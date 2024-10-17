/**
 * @file src/platform/windows/display_amd.cpp
 * @brief Display capture implementation using AMD Direct Capture
 */

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext_d3d11va.h>
}

#include "display.h"
#include "misc.h"
#include "src/config.h"
#include "src/main.h"
#include "src/video.h"

#include <AMF/components/DisplayCapture.h>
#include <AMF/components/FRC.h>
#include <AMF/components/VideoConverter.h>
#include <AMF/core/Trace.h>

namespace platf {
  using namespace std::literals;
}

namespace platf::dxgi {
  amd_capture_t::amd_capture_t() {
    capturedSurface = nullptr;
  }

  amd_capture_t::~amd_capture_t() {
    BOOST_LOG(debug) << "### R.G. ~amd_capture_t()";
    AMF_RESULT result;
    amf::AMFSurfacePtr output;
    // Before terminating the Display Capture component, we need to drain the remaining frames
    result = captureComp->Drain();
    if (result == AMF_OK) {
      do {
        result = captureComp->QueryOutput((amf::AMFData**) &output);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      } while (result != AMF_EOF);
    }
    captureComp->Terminate();
    frcComp->Flush();
    frcComp->Terminate();
    context->Terminate();
    // FreeLibrary((HMODULE) amfrt_lib.get());
  }

  capture_e
  amd_capture_t::release_frame() {
    if (capturedSurface != nullptr)
    {
        capturedSurface = nullptr;
    }

    return capture_e::ok;
  }

  /**
   * @brief Get the next frame from the producer thread.
   * If not available, the capture thread blocks until one is, or the wait times out.
   * @param timeout how long to wait for the next frame
   * @param out pointer to AMFSurfacePtr
   */
  capture_e
  amd_capture_t::next_frame(std::chrono::milliseconds timeout, amf::AMFData** out) {
    release_frame();

    AMF_RESULT result;
    auto capture_start = std::chrono::steady_clock::now();

    do {
      result = captureComp->QueryOutput(out);
      if (result == AMF_REPEAT) {
        if (std::chrono::steady_clock::now() - capture_start >= timeout) {
          BOOST_LOG(debug) << "### R.G. TIMEOUT captureComp->QueryOutput";
          return platf::capture_e::timeout;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    } while (result == AMF_REPEAT);

    if (result != AMF_OK) {
      BOOST_LOG(debug) << "### R.G. next_frame ERROR captureComp->QueryOutput(): " << result;
      //FIXME THIS MIGHT BE AN ISSUE
      return capture_e::timeout;
    }

    return capture_e::ok;
  }


  int
  amd_capture_t::init(display_base_t *display, const ::video::config_t &config, int output_index) {
    // We have to load AMF before calling the base init() because we will need it loaded
    // when our test_capture() function is called.
    amfrt_lib.reset(LoadLibraryW(AMF_DLL_NAME));
    if (!amfrt_lib) {
      // Probably not an AMD GPU system
      return -1;
    }

    auto fn_AMFQueryVersion = (AMFQueryVersion_Fn) GetProcAddress((HMODULE) amfrt_lib.get(), AMF_QUERY_VERSION_FUNCTION_NAME);
    auto fn_AMFInit = (AMFInit_Fn) GetProcAddress((HMODULE) amfrt_lib.get(), AMF_INIT_FUNCTION_NAME);

    if (!fn_AMFQueryVersion || !fn_AMFInit) {
      BOOST_LOG(error) << "Missing required AMF function!"sv;
      return -1;
    }

    auto result = fn_AMFQueryVersion(&amf_version);
    if (result != AMF_OK) {
      BOOST_LOG(error) << "AMFQueryVersion() failed: "sv << result;
      return -1;
    }

    // We don't support anything older than AMF 1.4.30. We'll gracefully fall back to DDAPI.
    if (amf_version < AMF_MAKE_FULL_VERSION(1, 4, 30, 0)) {
      BOOST_LOG(warning) << "AMD Direct Capture is not supported on AMF version"sv
                        << AMF_GET_MAJOR_VERSION(amf_version) << '.'
                        << AMF_GET_MINOR_VERSION(amf_version) << '.'
                        << AMF_GET_SUBMINOR_VERSION(amf_version) << '.'
                        << AMF_GET_BUILD_VERSION(amf_version);
      BOOST_LOG(warning) << "Consider updating your AMD graphics driver for better capture performance!"sv;
      return -1;
    }

    // Initialize AMF library
    result = fn_AMFInit(AMF_FULL_VERSION, &amf_factory);
    if (result != AMF_OK) {
      BOOST_LOG(error) << "AMFInit() failed: "sv << result;
      return -1;
    }

    DXGI_ADAPTER_DESC adapter_desc;
    display->adapter->GetDesc(&adapter_desc);

    amf::AMFTrace* traceAMF;
    amf_factory->GetTrace(&traceAMF);
    traceAMF->SetGlobalLevel(AMF_TRACE_DEBUG);
    traceAMF->EnableWriter(AMF_TRACE_WRITER_FILE, true);
    traceAMF->SetWriterLevel(AMF_TRACE_WRITER_FILE, AMF_TRACE_DEBUG);
    traceAMF->SetPath(L"D:/amflog.txt");

    amf::AMFDebug* debugAMF;
    amf_factory->GetDebug(&debugAMF);
    debugAMF->AssertsEnable(false);

    // Bail if this is not an AMD GPU
    if (adapter_desc.VendorId != 0x1002) {
      return -1;
    }

    // Create the capture context
    result = amf_factory->CreateContext(&context);

    if (result != AMF_OK) {
      BOOST_LOG(error) << "CreateContext() failed: "sv << result;
      return -1;
    }

    // Associate the context with our ID3D11Device. This will enable multithread protection on the device.
    result = context->InitDX11(display->device.get());
    if (result != AMF_OK) {
      BOOST_LOG(error) << "InitDX11() failed: "sv << result;
      return -1;
    }

    display->capture_format = DXGI_FORMAT_UNKNOWN;

    // Create the DisplayCapture component
    result = amf_factory->CreateComponent(context, AMFDisplayCapture, &(captureComp));
    if (result != AMF_OK) {
      BOOST_LOG(error) << "CreateComponent(AMFDisplayCapture) failed: "sv << result;
      return -1;
    }

    // Set parameters for non-blocking capture
    captureComp->SetProperty(AMF_DISPLAYCAPTURE_MONITOR_INDEX, output_index);
    captureComp->SetProperty(AMF_DISPLAYCAPTURE_FRAMERATE, AMFConstructRate(config.framerate/2, 1));
    BOOST_LOG(info) << "Framerate requested: " << config.framerate;

    captureComp->SetProperty(AMF_DISPLAYCAPTURE_MODE, AMF_DISPLAYCAPTURE_MODE_GET_CURRENT_SURFACE);
    captureComp->SetProperty(AMF_DISPLAYCAPTURE_DUPLICATEOUTPUT, false);

    // Initialize capture
    result = captureComp->Init(amf::AMF_SURFACE_UNKNOWN, 0, 0);
    if (result != AMF_OK) {
      BOOST_LOG(error) << "DisplayCapture::Init() failed: "sv << result;
      return -1;
    }

    amf::AMFSurfacePtr output;

    do {
      result = captureComp->QueryOutput((amf::AMFData**) &output);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (result != AMF_OK);


    captureComp->GetProperty(AMF_DISPLAYCAPTURE_FORMAT, &(capture_format));
    captureComp->GetProperty(AMF_DISPLAYCAPTURE_RESOLUTION, &(resolution));
    BOOST_LOG(info) << "Desktop capture format "sv << output->GetFormat();
    BOOST_LOG(info) << "Desktop resolution ["sv << resolution.width << 'x' << resolution.height << ']';

    BOOST_LOG(info) << "Using AMD Direct Capture API for display capture"sv;

    result = amf_factory->CreateComponent(context, AMFFRC, &(frcComp));
    if (result != AMF_OK) {
      BOOST_LOG(error) << "CreateComponent(AMFFRC) failed: "sv << result;
      return -1;
    }

    frcComp->SetProperty(AMF_FRC_ENGINE_TYPE, FRC_ENGINE_DX11);
    frcComp->SetProperty(AMF_FRC_MODE, FRC_x2_PRESENT);
    frcComp->SetProperty(AMF_FRC_ENABLE_FALLBACK, true);
    frcComp->SetProperty(AMF_FRC_INDICATOR, true);
    frcComp->SetProperty(AMF_FRC_PROFILE, FRC_PROFILE_SUPER);
    frcComp->SetProperty(AMF_FRC_MV_SEARCH_MODE, FRC_MV_SEARCH_NATIVE);
    frcComp->SetProperty(AMF_FRC_USE_FUTURE_FRAME, false);


    result = frcComp->Init((amf::AMF_SURFACE_FORMAT) capture_format, resolution.width, resolution.height);
    if (result != AMF_OK) {
      BOOST_LOG(error) << "FRC::Init() failed: "sv << result;
      return -1;
    }

    return 0;
  }

}  // namespace platf::dxgi
