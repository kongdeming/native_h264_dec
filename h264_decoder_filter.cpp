#include "h264_decoder_filter.h"

#include <cassert>

#include <boost/function.hpp>
#include <boost/bind.hpp>
#include <initguid.h>
#include <dvdmedia.h>
#include <mfapi.h>
#include <mferror.h>
#include <evr.h>

#include "ffmpeg.h"
#include "h264_decoder.h"
#include "chromium/base/win_util.h"
#include "common/dshow_util.h"
#include "common/hardware_env.h"
#include "common/intrusive_ptr_helper.h"
#include "utils.h"

using std::vector;
using boost::shared_ptr;
using boost::intrusive_ptr;
using boost::function;
using boost::bind;

namespace
{
inline int getDecodeSurfacesCount()
{
    return (win_util::GetWinVersion() >= win_util::WINVERSION_VISTA) ? 22 : 16;
}
}

CDXVA2Sample::CDXVA2Sample(CDXVA2Allocator *alloc, HRESULT* r)
    : CMediaSample(L"CDXVA2Sample", alloc, r, NULL, 0)
    , m_surface()
    , m_surfaceID(0)
{
}

CDXVA2Sample::~CDXVA2Sample()
{
}

HRESULT CDXVA2Sample::QueryInterface(const IID& ID, void** o)
{
    if (!o)
        return E_POINTER;

    if (IID_IMFGetService == ID)
    {
        IMFGetService* i = this;
        i->AddRef();
        *o = i;
        return S_OK;
    }

    if (__uuidof(IDXVA2Sample) == ID)
    {
        IMFGetService* i = this;
        i->AddRef();
        *o = i;
        return S_OK;
    }

    return CMediaSample::QueryInterface(ID, o);
}


ULONG CDXVA2Sample::AddRef()
{
    return CMediaSample::AddRef();
}

ULONG CDXVA2Sample::Release()
{
    // Return a temporary variable for thread safety.
    ULONG ref = CMediaSample::Release();
    return ref;
}

HRESULT CDXVA2Sample::GetService(const GUID& service, const IID& ID, void** o)
{
    if (service != MR_BUFFER_SERVICE)
        return MF_E_UNSUPPORTED_SERVICE;

    if (m_surface)
        return E_NOINTERFACE;

    return m_surface->QueryInterface(ID, o);
}

void CDXVA2Sample::setSurface(int surfaceID, IDirect3DSurface9* surface)
{
    m_surface = surface;
    m_surfaceID = surfaceID;
}

//------------------------------------------------------------------------------
CDXVA2Allocator::CDXVA2Allocator(CH264DecoderFilter* decoder,  HRESULT* r)
    : CBaseAllocator(L"CDXVA2Allocator", NULL, r)
    , m_decoder(decoder)
//     , m_surfaces(NULL)
//     , m_surfaceCount(0)
{
    assert(decoder);
}

CDXVA2Allocator::~CDXVA2Allocator()
{
    Free();
}

HRESULT CDXVA2Allocator::Alloc()
{
    CAutoLock lock(this);
    HRESULT r = CBaseAllocator::Alloc();
    if (FAILED(r))
        return r;

    // Free the old resources.
    Free();

    do
    {
        // Important : create samples in reverse order !
        for (m_lAllocated = m_lCount - 1; m_lAllocated >= 0;
            --m_lAllocated)
        {
            CDXVA2Sample* sample = new CDXVA2Sample(this, &r);
            if (FAILED(r))
                break;

            IDirect3DSurface9* surf = m_decoder->GetSurface(m_lAllocated);
            if (!surf)
            {
                r = E_UNEXPECTED;
                break;
            }

            // Assign the Direct3D surface pointer and the index.
            sample->setSurface(m_lAllocated, surf);

            // Add to the sample list.
            m_lFree.Add(sample);
        }
        if (FAILED(r))
            break;

        m_bChanged = FALSE;
        return r;
    } while (0);

    Free();
    return r;
}

void CDXVA2Allocator::Free()
{
    CMediaSample* sample = NULL;
    m_decoder->FlushDXVADecoder();

    do
    {
        sample = m_lFree.RemoveHead();
        if (sample)
            delete sample;

    } while (sample);

    m_lAllocated = 0;
}

//------------------------------------------------------------------------------
namespace
{
const wchar_t* outputPinName = L"CH264DecoderOutputPin";
const wchar_t* inputPinName = L"CH264DecoderInputPin";
}

CH264DecoderOutputPin::CH264DecoderOutputPin(CH264DecoderFilter* decoder, HRESULT* r)
    : CTransformOutputPin(outputPinName, decoder, r, outputPinName)
    , m_decoder(decoder)
    , m_allocator(NULL)
    , m_DXVA1SurfCount(0)
    , m_DXVA1DecoderID(GUID_NULL)
    , m_uncompPixelFormat()
{
    memset(&m_uncompPixelFormat, 0, sizeof(m_uncompPixelFormat));
}

CH264DecoderOutputPin::~CH264DecoderOutputPin()
{
}

HRESULT CH264DecoderOutputPin::NonDelegatingQueryInterface(const IID& ID,
                                                             void** o)
{
    if (!o)
        return E_POINTER;

    if (IID_IAMVideoAcceleratorNotify == ID)
    {
        IAMVideoAcceleratorNotify* i = this;
        i->AddRef();
        *o = i;
        return S_OK;
    }

    return CTransformOutputPin::NonDelegatingQueryInterface(ID, o);
}

HRESULT CH264DecoderOutputPin::GetUncompSurfacesInfo(
    const GUID* profileID, AMVAUncompBufferInfo* uncompBufInfo)
{
    HRESULT r = E_INVALIDARG;
    if (m_decoder->IsFormatSupported(*profileID))
    {
        intrusive_ptr<IAMVideoAccelerator> accel;
        IPin* connected = GetConnected();
        if (!connected)
            return E_UNEXPECTED;

        r = connected->QueryInterface(IID_IAMVideoAccelerator,
                                      reinterpret_cast<void**>(&accel));
        if (SUCCEEDED(r) && accel)
        {
            const int surfCount = getDecodeSurfacesCount();
            uncompBufInfo->dwMaxNumSurfaces = surfCount;
            uncompBufInfo->dwMinNumSurfaces = surfCount;
            r = m_decoder->ConfirmDXVA1UncompFormat(
                accel.get(), profileID,
                &uncompBufInfo->ddUncompPixelFormat);
            if (SUCCEEDED(r))
            {
                memcpy(&m_uncompPixelFormat,
                       &uncompBufInfo->ddUncompPixelFormat,
                       sizeof(m_uncompPixelFormat));
                m_DXVA1DecoderID = *profileID;
            }
        }
    }

    return r;
}

HRESULT CH264DecoderOutputPin::SetUncompSurfacesInfo(
    DWORD actualUncompSurfacesAllocated)
{
    m_DXVA1SurfCount = actualUncompSurfacesAllocated;
    return S_OK;
}

#define DXVA_RESTRICTED_MODE_H264_E             0x68
HRESULT CH264DecoderOutputPin::GetCreateVideoAcceleratorData(
    const GUID* profileID, DWORD* miscDataSize, void** miscData)
{
    IPin* connected = GetConnected();
    if (!connected)
        return E_UNEXPECTED;

    intrusive_ptr<IAMVideoAccelerator> accel;
    HRESULT r = connected->QueryInterface(IID_IAMVideoAccelerator,
                                          reinterpret_cast<void**>(&accel));
    if (FAILED(r))
        return r;

    AMVAUncompDataInfo uncompDataInfo;
    memcpy(&uncompDataInfo.ddUncompPixelFormat, &m_uncompPixelFormat,
           sizeof(m_uncompPixelFormat));
    uncompDataInfo.dwUncompWidth = 720;
    uncompDataInfo.dwUncompHeight = 480;

    AMVACompBufferInfo compInfo[30];
    DWORD numTypesCompBuffers = arraysize(compInfo);
    r = accel->GetCompBufferInfo(&m_DXVA1DecoderID, &uncompDataInfo,
                                 &numTypesCompBuffers, compInfo);
    if (FAILED(r))
        return r;

    r = m_decoder->ActivateDXVA1(accel.get(), profileID, uncompDataInfo,
                                 m_DXVA1SurfCount);
    if (SUCCEEDED(r))
    {
        m_decoder->SetDXVA1PixelFormat(m_uncompPixelFormat);
        DXVA_ConnectMode* connectMode =
            reinterpret_cast<DXVA_ConnectMode*>(
                CoTaskMemAlloc(sizeof(DXVA_ConnectMode)));
        connectMode->guidMode = m_DXVA1DecoderID;
        connectMode->wRestrictedMode = DXVA_RESTRICTED_MODE_H264_E;
        *miscDataSize = sizeof(*connectMode);
        *miscData = connectMode;
    }

    return r;
}

HRESULT CH264DecoderOutputPin::InitAllocator(IMemAllocator** alloc)
{
    if (m_decoder->NeedCustomizeAllocator())
    {
        HRESULT r = S_FALSE;
        intrusive_ptr<CDXVA2Allocator> a = new CDXVA2Allocator(m_decoder, &r);
        if (FAILED(r))
            return r;

        m_allocator = a;

        // Return the IMemAllocator interface.
        return m_allocator->QueryInterface(IID_IMemAllocator,
                                           reinterpret_cast<void**>(alloc));
    }

    return CTransformOutputPin::InitAllocator(alloc);
}

//------------------------------------------------------------------------------
namespace
{
struct { const GUID& SubType; int PlaneCount; int FourCC; } supportedFormats[] =
{
    // Hardware formats
    { DXVA2_ModeH264_E, 1, MAKEFOURCC('d','x','v','a') },
    { DXVA2_ModeH264_F, 1, MAKEFOURCC('d','x','v','a') },
    { MEDIASUBTYPE_NV12, 1, MAKEFOURCC('d','x','v','a') },
    { MEDIASUBTYPE_NV12, 1, MAKEFOURCC('D','X','V','A') },
    { MEDIASUBTYPE_NV12, 1, MAKEFOURCC('D','x','V','A') },
    { MEDIASUBTYPE_NV12, 1, MAKEFOURCC('D','X','v','A') },

    // Software formats
    { MEDIASUBTYPE_YV12, 3, MAKEFOURCC('Y','V','1','2') },
    { MEDIASUBTYPE_YUY2, 3, MAKEFOURCC('Y','U','Y','2') }
};

enum KDXVAH264Compatibility
{
    DXVA_UNSUPPORTED_LEVEL = 1,
    DXVA_TOO_MUCH_REF_FRAMES = 2,
    DXVA_INCOMPATIBLE_SAR = 4
};

bool hasDriverVersionReached(LARGE_INTEGER version, int a, int b, int c, int d)
{
    if (HIWORD(version.HighPart) > a)
        return true;
    
    if (HIWORD(version.HighPart) == a)
    {
        if (LOWORD(version.HighPart) > b)
            return true;
        
        if (LOWORD(version.HighPart) == b)
        {
            if (HIWORD(version.LowPart) > c)
                return true;

            if (HIWORD(version.LowPart) == c)
                if (LOWORD(version.LowPart) >= d)
                    return true;
        }
    }

    return false;
}

int checkHWCompatibilityForH264(int width, int height, int videoLevel,
                                int refFrameCount)
{
    int noLevel51Support = 1;
    int tooMuchRefFrames = 0;
    int maxRefFrames = 0;
    if (videoLevel >= 0)
    {
        int vendor = CHardwareEnv::get()->GetVideoCardVendor();
        int device = CHardwareEnv::get()->GetVideoCardDeviceID();
        LARGE_INTEGER videoDriverVersion;
        videoDriverVersion.QuadPart =
            CHardwareEnv::get()->GetVideoCardDriverVersion();

        const int maxRefFramesDPB41 = std::min(11, 8388608 / (width * height));
        maxRefFrames = maxRefFramesDPB41; // default value is calculate
        if (CHardwareEnv::PCI_VENDOR_NVIDIA == vendor)
        {
            // nVidia cards support level 5.1 since drivers v6.14.11.7800 for
            // XP and drivers v7.15.11.7800 for Vista/7
            if (win_util::GetWinVersion() >= win_util::WINVERSION_VISTA)
            {
                if (hasDriverVersionReached(videoDriverVersion, 7, 15, 11,
                                            7800))
                {
                    noLevel51Support = 0;

                    // max ref frames is 16 for HD and 11 otherwise
                    if (width >= 1280)
                        maxRefFrames = 16;
                    else
                        maxRefFrames = 11;
                }
            }
            else
            {
                if (hasDriverVersionReached(videoDriverVersion, 6, 14, 11,
                                            7800))
                {
                    noLevel51Support = 0;

                    // max ref frames is 14
                    maxRefFrames = 14;
                }
            }
        }
        else if (CHardwareEnv::PCI_VENDOR_S3_GRAPHICS == vendor)
        {
            noLevel51Support = 0;
        }
        else if (CHardwareEnv::PCI_VENDOR_ATI == vendor)
        {
            // HD4xxx and HD5xxx ATI cards support level 5.1 since drivers
            // v8.14.1.6105 (Catalyst 10.4)
            if ((0x68 == (device >> 8)) || (0x94 == (device >> 8)))
            {
                if (hasDriverVersionReached(videoDriverVersion, 8, 14, 1, 6105))
                {
                    noLevel51Support = 0;
                    maxRefFrames = 16;
                }
            }
        }

        // Check maximum allowed number reference frames.
        if (refFrameCount > maxRefFrames)
            tooMuchRefFrames = 1;
    }

    int hasVideoLevelReached51 = (videoLevel >= 51) ? 1 : 0;
    return (hasVideoLevelReached51 * noLevel51Support *
        DXVA_UNSUPPORTED_LEVEL) + (tooMuchRefFrames * DXVA_TOO_MUCH_REF_FRAMES);
}
}

CUnknown* CH264DecoderFilter::CreateInstance(IUnknown* aggregator, HRESULT *r)
{
    return new CH264DecoderFilter(aggregator, r);
}

CH264DecoderFilter::~CH264DecoderFilter()
{
}

HRESULT CH264DecoderFilter::CheckInputType(const CMediaType* inputType)
{
    if (!inputType)
        return E_POINTER;

    if (MEDIATYPE_Video != *inputType->Type())
        return VFW_E_TYPE_NOT_ACCEPTED;

    if (CFFMPEG::get()->IsSubTypeSupported(*inputType))
        return S_OK;

    return VFW_E_TYPE_NOT_ACCEPTED;
}

HRESULT CH264DecoderFilter::CheckTransform(const CMediaType* inputType, 
                                           const CMediaType* outputType)
{
    HRESULT r = CheckInputType(inputType);
    if (FAILED(r))
        return r;

    if (!outputType)
        return E_POINTER;

    if (MEDIATYPE_Video != *outputType->Type())
        return VFW_E_TYPE_NOT_ACCEPTED;

    if ((MEDIASUBTYPE_YV12 == *inputType->Subtype()) ||
        (MEDIASUBTYPE_I420 == *inputType->Subtype()) ||
        (MEDIASUBTYPE_IYUV == *inputType->Subtype()))
    {
        if ((MEDIASUBTYPE_YV12 != *outputType->Subtype()) &&
            (MEDIASUBTYPE_I420 != *outputType->Subtype()) &&
            (MEDIASUBTYPE_IYUV != *outputType->Subtype()) &&
            (MEDIASUBTYPE_YUY2 != *outputType->Subtype()))
            return VFW_E_TYPE_NOT_ACCEPTED;
    }
    else if (MEDIASUBTYPE_YUY2 == *inputType->Subtype())
    {
        if (MEDIASUBTYPE_YUY2 != *outputType->Subtype())
            return VFW_E_TYPE_NOT_ACCEPTED;
    }

    return S_OK;
}

HRESULT CH264DecoderFilter::DecideBufferSize(IMemAllocator * allocator, 
                                             ALLOCATOR_PROPERTIES* prop)
{
    BITMAPINFOHEADER header;
    if (!ExtractBitmapInfoFromMediaType(m_pOutput->CurrentMediaType(), &header))
        return E_FAIL;

    if (!prop)
        return E_POINTER;

    ALLOCATOR_PROPERTIES requested = *prop;
    if (requested.cbAlign < 1) 
        requested.cbAlign = 1;

    if (m_decoder->NeedCustomizeAllocator())
        requested.cBuffers = getDecodeSurfacesCount();
    else if (requested.cBuffers < 1) 
        requested.cBuffers = 1;

    requested.cbBuffer = header.biSizeImage;
    requested.cbPrefix = 0;

    ALLOCATOR_PROPERTIES actual;
    HRESULT r = allocator->SetProperties(&requested, &actual);
    if (FAILED(r)) 
        return r;

    return (requested.cBuffers > actual.cBuffers) ||
        (requested.cbBuffer > actual.cbBuffer) ? E_FAIL : S_OK;
}

HRESULT CH264DecoderFilter::GetMediaType(int position, CMediaType* mediaType)
{
    if (position < 0)
        return E_INVALIDARG;

    if (!mediaType)
        return E_POINTER;

    if (position >= static_cast<int>(m_mediaTypes.size()))
        return VFW_S_NO_MORE_ITEMS;

    *mediaType = *m_mediaTypes[position];
    return S_OK;
}

HRESULT CH264DecoderFilter::SetMediaType(PIN_DIRECTION dir,
                                         const CMediaType* mediaType)
{
    if (PINDIR_INPUT == dir)
    {
        if (!mediaType)
            return E_POINTER;

        // Rebuild output media types.
        m_mediaTypes.clear();

        // Get dimension info.
        int width;
        int height;
        int aspectX;
        int aspectY;
        if (!ExtractDimensionFromMediaType(*mediaType, &width, &height,
                                           &aspectX, &aspectY))
            return VFW_E_TYPE_NOT_ACCEPTED;

        // Get bitmap info.
        BITMAPINFOHEADER bitmapHeader;
        if (!ExtractBitmapInfoFromMediaType(*mediaType, &bitmapHeader))
            return VFW_E_TYPE_NOT_ACCEPTED;

        bitmapHeader.biWidth = width;
        bitmapHeader.biHeight = height;
        bitmapHeader.biBitCount = 12;
        bitmapHeader.biSizeImage =
            bitmapHeader.biWidth * bitmapHeader.biHeight *
            bitmapHeader.biBitCount >> 3;

        VIDEOINFOHEADER* inputFormat =
            reinterpret_cast<VIDEOINFOHEADER*>(mediaType->Format());
        if (!inputFormat)
            return E_UNEXPECTED;

        m_averageTimePerFrame = inputFormat->AvgTimePerFrame;

        // Type 1: FORMAT_VideoInfo
        VIDEOINFOHEADER header = {0};
        header.bmiHeader = bitmapHeader;
        header.bmiHeader.biXPelsPerMeter = width * aspectY;
        header.bmiHeader.biYPelsPerMeter = height * aspectX;
        header.AvgTimePerFrame = inputFormat->AvgTimePerFrame;
        header.dwBitRate = inputFormat->dwBitRate;
        header.dwBitErrorRate = inputFormat->dwBitErrorRate;

        // Type 2: FORMAT_VideoInfo2
        VIDEOINFOHEADER2 header2 = {0};
        header2.bmiHeader = bitmapHeader;
        header2.dwPictAspectRatioX = aspectX;
        header2.dwPictAspectRatioY = aspectY;
        header2.dwInterlaceFlags =
            AMINTERLACE_IsInterlaced | AMINTERLACE_DisplayModeBobOrWeave;
        header2.AvgTimePerFrame = inputFormat->AvgTimePerFrame;
        header2.dwBitRate = inputFormat->dwBitRate;
        header2.dwBitErrorRate = inputFormat->dwBitErrorRate;

        // Copy source and target rectangles from input pin.
        if (inputFormat->rcSource.right && inputFormat->rcSource.bottom)
        {
            header.rcSource = inputFormat->rcSource;
            header.rcTarget = inputFormat->rcTarget;
            header2.rcSource = inputFormat->rcSource;
            header2.rcTarget = inputFormat->rcTarget;
        }
        else
        {
            header.rcSource.right = width;
            header.rcTarget.right = width;
            header.rcSource.bottom = height;
            header.rcTarget.bottom = height;
            header2.rcSource.right = width;
            header2.rcTarget.right = width;
            header2.rcSource.bottom = height;
            header2.rcTarget.bottom = height;
        }

        for (int i = 0; i < arraysize(supportedFormats); ++i)
        {
            shared_ptr<CMediaType> myType(new CMediaType);
            myType->SetType(&MEDIATYPE_Video);
            myType->SetSubtype(&supportedFormats[i].SubType);
            myType->SetFormatType(&FORMAT_VideoInfo);

            header.bmiHeader.biPlanes = supportedFormats[i].PlaneCount;
            header.bmiHeader.biCompression = supportedFormats[i].FourCC;
            myType->SetFormat(reinterpret_cast<BYTE*>(&header), sizeof(header));

            m_mediaTypes.push_back(myType);

            shared_ptr<CMediaType> myType2(new CMediaType(*myType));
            myType2->SetFormatType(&FORMAT_VideoInfo2);

            header2.bmiHeader.biPlanes = supportedFormats[i].PlaneCount;
            header2.bmiHeader.biCompression = supportedFormats[i].FourCC;
            myType2->SetFormat(reinterpret_cast<BYTE*>(&header2),
                               sizeof(header2));

            m_mediaTypes.push_back(myType2);
        }
    }

    return S_OK;
}

HRESULT CH264DecoderFilter::CompleteConnect(PIN_DIRECTION dir, IPin* receivePin)
{
    if (PINDIR_INPUT == dir)
    {
        m_preDecode = CFFMPEG::get()->CreateCodec(m_pInput->CurrentMediaType());
        if (!m_preDecode)
            return VFW_E_TYPE_NOT_ACCEPTED;
    }
    else if (PINDIR_OUTPUT == dir)
    {
        if (m_decoder) // DXVA1 has been activated.
        {
            if (!m_decoder->Init(m_pixelFormat, m_averageTimePerFrame))
                m_decoder.reset();
        }
        
        if (!m_decoder) // Not support DXVA1.
        {
            if (!SUCCEEDED(ActivateDXVA2()))
                m_decoder.reset(new CH264SWDecoder(m_preDecode.get()));
        }
    }

    return CTransformFilter::CompleteConnect(dir, receivePin);
}

HRESULT CH264DecoderFilter::BreakConnect(PIN_DIRECTION dir)
{
    if (PINDIR_INPUT == dir)
    {
        m_decoder.reset();
        m_preDecode.reset();
    }

    return S_OK;
}

HRESULT CH264DecoderFilter::NewSegment(REFERENCE_TIME start,
                                       REFERENCE_TIME stop, double rate)
{
    {
        AutoLock lock(m_decodeAccess);
        m_preDecode->FlushBuffers();
        if (m_decoder)
            m_decoder->Flush();
    }

    return CTransformFilter::NewSegment(start, stop, rate);
}

HRESULT CH264DecoderFilter::Receive(IMediaSample* inSample)
{
    AM_SAMPLE2_PROPERTIES* const props = m_pInput->SampleProps();
    if (props->dwStreamId != AM_STREAM_MEDIA)
        return m_pOutput->Deliver(inSample);

    assert(m_decoder);
    if (!m_decoder)
        return E_UNEXPECTED;

    BYTE* data;
    HRESULT r = inSample->GetPointer(&data);
    if (FAILED(r))
        return r;

    const int dataLength = inSample->GetActualDataLength();
    const int minDataSize = dataLength + CFFMPEG::GetInputBufferPaddingSize();
    if (inSample->GetSize() < minDataSize)
    {
        assert(false);
        // Reconfigure sample size.
    }

    // Make sure the padding bytes are initialized to 0.
    memset(data + dataLength, 0, CFFMPEG::GetInputBufferPaddingSize());

    REFERENCE_TIME start;
    REFERENCE_TIME stop;
    r = inSample->GetTime(&start, &stop);
    if (FAILED(r))
        return r;

    if ((stop <= start) && (stop != std::numeric_limits<int64>::min()))
        stop = start + m_averageTimePerFrame;

    m_preDecode->UpdateTime(start, stop);

    const int8* dataStart = reinterpret_cast<const int8*>(data);
    int dataRemaining = dataLength;
    while (dataRemaining > 0)
    {
        intrusive_ptr<IMediaSample> outSample;
        r = InitializeOutputSample(
            inSample, reinterpret_cast<IMediaSample**>(&outSample));
        if (FAILED(r))
            return r;

        int usedBytes = 0;
        {
            AutoLock lock(m_decodeAccess);
            r = m_decoder->Decode(dataStart, dataRemaining, start, stop,
                outSample.get(), &usedBytes);
            if (S_FALSE == r)
                return S_OK;

            if (FAILED(r))
                return r;

            r = m_decoder->DisplayNextFrame(outSample.get());
        }
        if (E_NOTIMPL == r)
            r = m_pOutput->Deliver(outSample.get());

        if (FAILED(r))
            return r;

        dataRemaining -= usedBytes;
        dataStart += usedBytes;
    }

    return r;
}

HRESULT CH264DecoderFilter::ActivateDXVA1(IAMVideoAccelerator* accel,
                                          const GUID* decoderID,
                                          const AMVAUncompDataInfo& uncompInfo,
                                          int surfaceCount)
{
    if (!m_preDecode)
        return E_FAIL;

    if (!accel || !decoderID)
        return E_POINTER;

    if (m_decoder && (m_decoder->GetDecoderID() == *decoderID))
        return S_OK;

    m_decoder.reset();
    int campatible = checkHWCompatibilityForH264(
        m_preDecode->GetWidth(), m_preDecode->GetHeight(),
        m_preDecode->GetVideoLevel(), m_preDecode->GetRefFrameCount());
    if (DXVA_UNSUPPORTED_LEVEL == campatible)
        return E_FAIL;

    m_decoder.reset(new CH264DXVA1Decoder(*decoderID, m_preDecode.get(), accel,
                                          surfaceCount));
    return S_OK;
}

HRESULT CH264DecoderFilter::ActivateDXVA2()
{
    IPin* pin = m_pOutput->GetConnected();
    if (!pin)
        return VFW_E_NOT_CONNECTED;

    intrusive_ptr<IMFGetService> getService;
    HRESULT r = pin->QueryInterface(IID_IMFGetService,
                                    reinterpret_cast<void**>(&getService));
    if (FAILED(r))
        return r;

    intrusive_ptr<IDirect3DDeviceManager9> devManager;
    r = getService->GetService(MR_VIDEO_ACCELERATION_SERVICE,
                               IID_IDirect3DDeviceManager9,
                               reinterpret_cast<void**>(&devManager));
    if (FAILED(r))
        return r;

    HANDLE device;
    r = devManager->OpenDeviceHandle(&device);
    if (FAILED(r))
        return r;

    function<HRESULT (HANDLE)> closeMethod =
        bind(&IDirect3DDeviceManager9::CloseDeviceHandle, devManager, _1);
    shared_ptr<void> deviceHandle(device, closeMethod);

    intrusive_ptr<IDirectXVideoDecoderService> decoderService;
    r = devManager->GetVideoService(device, IID_IDirectXVideoDecoderService,
                                    reinterpret_cast<void**>(&decoderService));
    if (FAILED(r))
        return r;

    UINT devGUIDCount;
    GUID* devGUIDs;
    r = decoderService->GetDecoderDeviceGuids(&devGUIDCount, &devGUIDs);
    if (FAILED(r))
        return r;

    shared_ptr<void> autoReleaseGUID(devGUIDs, CoTaskMemFree);
    for (int i = 0; i < static_cast<int>(devGUIDCount); ++i)
    {
        if (!IsFormatSupported(devGUIDs[i]))
            continue;

        DXVA2_ConfigPictureDecode selectedConfig;
        DXVA2_VideoDesc selectedFormat;
        r = ConfirmDXVA2UncompFormat(decoderService.get(), &devGUIDs[i],
                                     &selectedConfig, &selectedFormat);
        if (FAILED(r))
            continue;

        r = configureEVRForDXVA2(getService.get());
        if (FAILED(r))
            return r;

        m_devManager = devManager;
        m_deviceHandle = deviceHandle;
        m_decoderService = decoderService;
        m_config = selectedConfig;
        return CreateDXVA2Decoder(devGUIDs[i], selectedFormat);
    }

    return E_FAIL;
}

HRESULT CH264DecoderFilter::CreateDXVA2Decoder(const GUID& decoderID,
                                               const DXVA2_VideoDesc& videoDesc)
{
    assert(m_devManager);
    intrusive_ptr<IDirectXVideoAccelerationService> accelService;
    HRESULT r = m_devManager->GetVideoService(
        reinterpret_cast<HANDLE>(m_deviceHandle.get()),
        IID_IDirectXVideoAccelerationService,
        reinterpret_cast<void**>(&accelService));
    if (FAILED(r))
        return r;

    // Allocate a new array of pointers.
    const int surfaceCount = getDecodeSurfacesCount();
    scoped_array<IDirect3DSurface9*> surfaces(
        new IDirect3DSurface9*[surfaceCount]);

    // Allocate the surfaces.
    r = accelService->CreateSurface(m_preDecode->GetWidth(),
                                    m_preDecode->GetHeight(), surfaceCount - 1,
                                    videoDesc.Format, D3DPOOL_DEFAULT, 0,
                                    DXVA2_VideoDecoderRenderTarget,
                                    surfaces.get(), NULL);
    if (FAILED(r))
        return r;

    // Get the surfaces managed.
    std::vector<boost::intrusive_ptr<IDirect3DSurface9> > surfList;
    for (int i = 0; i < surfaceCount; ++i)
        surfList.push_back(
            intrusive_ptr<IDirect3DSurface9>(surfaces[i], false));

    intrusive_ptr<IDirectXVideoDecoder> accel;
    r = m_decoderService->CreateVideoDecoder(
        decoderID, &videoDesc, &m_config, surfaces.get(), surfaceCount,
        reinterpret_cast<IDirectXVideoDecoder**>(&accel));
    if (FAILED(r))
        return r;

    m_decoder.reset(
        new CH264DXVA2Decoder(decoderID, m_preDecode.get(), accel.get()));
    return r;
}

bool CH264DecoderFilter::IsFormatSupported(const GUID& formatID)
{
    for (int i = 0; i < arraysize(supportedFormats); ++i)
        if (formatID == supportedFormats[i].SubType)
            return true;

    return false;
}

HRESULT CH264DecoderFilter::ConfirmDXVA1UncompFormat(IAMVideoAccelerator* accel,
                                                     const GUID* decoderID,
    DDPIXELFORMAT* pixelFormat)
{
    assert(accel);
    assert(pixelFormat);
    DWORD formatCount = 0;
    HRESULT r = accel->GetUncompFormatsSupported(decoderID, &formatCount, NULL);
    if (FAILED(r))
        return r;

    if (formatCount < 0)
        return E_FAIL;

    scoped_array<DDPIXELFORMAT> formats(new DDPIXELFORMAT[formatCount]);
    r = accel->GetUncompFormatsSupported(decoderID, &formatCount,
        formats.get());
    if (FAILED(r))
        return r;

    for (DWORD i = 0; i < formatCount; ++i)
    {
        if (formats[i].dwFourCC != MAKEFOURCC('N', 'V', '1', '2'))
            continue;

        memcpy(pixelFormat, &formats[i], sizeof(*pixelFormat));
        return S_OK;
    }

    return E_FAIL;
}

HRESULT CH264DecoderFilter::ConfirmDXVA2UncompFormat(
    IDirectXVideoDecoderService* decoderService, const GUID* decoderID,
    DXVA2_ConfigPictureDecode* selectedConfig, DXVA2_VideoDesc* selectedFormat)
{
    assert(selectedConfig);
    assert(selectedFormat);
    UINT formatCount;
    D3DFORMAT* formats;
    HRESULT r = decoderService->GetDecoderRenderTargets(*decoderID, 
                                                        &formatCount, &formats);
    if (FAILED(r))
        return r;

    shared_ptr<void> autoReleaseFormats(formats, CoTaskMemFree);
    for (int i = 0; i < static_cast<int>(formatCount); ++i)
    {
        if (MAKEFOURCC('N', 'V', '1', '2') != formats[i])
            continue;

        // Get the available configurations.
        DXVA2_VideoDesc desc = {0};
        desc.SampleWidth = m_preDecode->GetWidth();
        desc.SampleHeight = m_preDecode->GetHeight();
        desc.UABProtectionLevel = 1;
        desc.Format = formats[i];

        UINT configCount;
        DXVA2_ConfigPictureDecode* configs;
        r = decoderService->GetDecoderConfigurations(*decoderID, &desc, NULL,
                                                     &configCount, &configs);
        if (FAILED(r))
            continue;

        shared_ptr<void> autoReleaseConfigs(configs, CoTaskMemFree);
        *selectedFormat = desc;

        // Find a supported configuration.
        for (int j = 0; j < static_cast<int>(configCount); ++j)
        {
            *selectedConfig = configs[j];
            if (2 == configs[j].ConfigBitstreamRaw)
                return S_OK;
        }

        return configCount ? S_OK : E_FAIL;
    }

    return E_FAIL;
}

void CH264DecoderFilter::SetDXVA1PixelFormat(const DDPIXELFORMAT& pixelFormat)
{
    memcpy(&m_pixelFormat, &pixelFormat, sizeof(m_pixelFormat));
}

bool CH264DecoderFilter::NeedCustomizeAllocator()
{
    return (m_decoder && m_decoder->NeedCustomizeAllocator());
}

IDirect3DSurface9* CH264DecoderFilter::GetSurface(int n)
{
    return
        (n < static_cast<int>(m_surfaces.size())) ? m_surfaces[n].get() : NULL;
}

CH264DecoderFilter::CH264DecoderFilter(IUnknown* aggregator, HRESULT* r)
    : CTransformFilter(L"H264DecodeFilter", aggregator, CLSID_NULL)
    , m_mediaTypes()
    , m_preDecode()
    , m_devManager()
    , m_pixelFormat()
    , m_decodeAccess()
    , m_decoder()
    , m_averageTimePerFrame(1)
{
    memset(&m_pixelFormat, 0, sizeof(m_pixelFormat));

    if (m_pInput)
        delete m_pInput;

    m_pInput = new CTransformInputPin(inputPinName, this, r, inputPinName);

    if (m_pOutput)
        delete m_pOutput;

    m_pOutput = new CH264DecoderOutputPin(this, r);
    if (r && FAILED(*r))
        return;
}

HRESULT CH264DecoderFilter::configureEVRForDXVA2(IMFGetService* getService)
{
    assert(getService);
    intrusive_ptr<IDirectXVideoMemoryConfiguration> videoConfig;
    HRESULT r = getService->GetService(MR_VIDEO_ACCELERATION_SERVICE,
                                       IID_IDirectXVideoMemoryConfiguration,
                                       reinterpret_cast<void**>(&videoConfig));
    if (FAILED(r))
        return r;

    for (int i = 0; ; ++i)
    {
        DXVA2_SurfaceType surfType;
        r = videoConfig->GetAvailableSurfaceTypeByIndex(i, &surfType);
        if (FAILED(r))
            break;

        if (DXVA2_SurfaceType_DecoderRenderTarget == surfType)
            return videoConfig->SetSurfaceType(surfType);
    }

    return r;
}