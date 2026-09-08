#define main tb330fu_capture_probe_main
#include "../capture/tb330fu-camera-capture.cpp"
#undef main

int main(int argc, char** argv) {
    const char* output = argc > 1 ? argv[1] : "/run/tb330fu/frame-raw16-characteristics.json";
    ABinderProcess_setThreadPoolMaxThreadCount(4);
    ABinderProcess_startThreadPool();

    const char* provider_name = "android.hardware.camera.provider.ICameraProvider/internal/0";
    ndk::SpAIBinder binder(AServiceManager_checkService(provider_name));
    if (!binder.get()) { std::fprintf(stderr, "provider_lookup failed\n"); return 10; }
    auto provider = aidl::android::hardware::camera::provider::ICameraProvider::fromBinder(binder);
    if (!provider) { std::fprintf(stderr, "provider_from_binder failed\n"); return 11; }

    std::vector<std::string> ids;
    auto s = provider->getCameraIdList(&ids);
    if (!s.isOk()) { std::fprintf(stderr, "get_camera_id_list failed: %s\n", s.getDescription().c_str()); return 12; }
    std::printf("camera_id_count=%zu\n", ids.size());
    for (const auto& id : ids) std::printf("camera_id=%s\n", id.c_str());

    const std::string requested = argc > 2 ? argv[2] : "device@1.1/internal/0";
    std::shared_ptr<ICameraDevice> device;
    s = provider->getCameraDeviceInterface(requested, &device);
    if (!s.isOk() || !device) { std::fprintf(stderr, "get_camera_device_interface failed: %s\n", s.getDescription().c_str()); return 13; }
    CameraMetadata characteristics;
    s = device->getCameraCharacteristics(&characteristics);
    if (!s.isOk()) { std::fprintf(stderr, "get_camera_characteristics failed: %s\n", s.getDescription().c_str()); return 14; }
    std::FILE* f = std::fopen(output, "w");
    if (!f) { std::perror("characteristics_output"); return 15; }
    json_metadata(f, characteristics.metadata);
    std::fclose(f);
    const auto* metadata = reinterpret_cast<const camera_metadata_t*>(characteristics.metadata.data());
    std::printf("camera_characteristics_camera=%s\ncharacteristics_dump=%s\ncharacteristics_entry_count=%zu\n",
                requested.c_str(), output, get_camera_metadata_entry_count(metadata));
    return 0;
}
