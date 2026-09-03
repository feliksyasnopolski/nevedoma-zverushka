#include "tb330fu_sensor_feed.h"
#include <aidl/android/hardware/sensors/BnSensorsCallback.h>
#include <aidl/android/hardware/sensors/Event.h>
#include <aidl/android/hardware/sensors/ISensors.h>
#include <aidl/android/hardware/sensors/SensorInfo.h>
#include <aidl/android/hardware/sensors/SensorType.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <fmq/AidlMessageQueue.h>
#include <fmq/EventFlag.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <thread>
#include <vector>
#include <unistd.h>

using namespace aidl::android::hardware::sensors;
using android::AidlMessageQueue;
using android::hardware::EventFlag;
using aidl::android::hardware::common::fmq::SynchronizedReadWrite;
using EventQueue = AidlMessageQueue<Event, SynchronizedReadWrite>;
using WakeQueue = AidlMessageQueue<int32_t, SynchronizedReadWrite>;
static std::atomic<bool> stopping{false};
static void stop_handler(int) { stopping.store(true); }

class Callback final : public BnSensorsCallback {
public:
    ndk::ScopedAStatus onDynamicSensorsConnected(const std::vector<SensorInfo>&) override { return ndk::ScopedAStatus::ok(); }
    ndk::ScopedAStatus onDynamicSensorsDisconnected(const std::vector<int32_t>&) override { return ndk::ScopedAStatus::ok(); }
};

static bool send_sample(int fd, uint32_t kind, int64_t timestamp, const Event& event)
{
    tb330fu_sensor_sample sample{};
    sample.version = TB330FU_SENSOR_ABI_VERSION;
    sample.kind = kind;
    sample.timestamp_ns = timestamp > 0 ? static_cast<__u64>(timestamp) : 1;
    if (kind == TB330FU_SENSOR_LIGHT || kind == TB330FU_SENSOR_STEPS) {
        sample.light = static_cast<__s32>(event.payload.get<Event::EventPayload::Tag::scalar>());
    } else {
        auto v = event.payload.get<Event::EventPayload::Tag::vec3>();
        sample.vec3.x = static_cast<__s32>(v.x * 1000000.0f);
        sample.vec3.y = static_cast<__s32>(v.y * 1000000.0f);
        sample.vec3.z = static_cast<__s32>(v.z * 1000000.0f);
    }
    return ::write(fd, &sample, sizeof(sample)) == static_cast<ssize_t>(sizeof(sample));
}

static int64_t period_for(const SensorInfo& info, int64_t target_ns)
{
    int64_t minimum = std::max<int64_t>(0, info.minDelayUs) * 1000LL;
    return std::max(target_ns, minimum);
}

int main()
{
    std::signal(SIGTERM, stop_handler); std::signal(SIGINT, stop_handler);
    int feed = ::open("/dev/tb330fu-sensor-feed", O_WRONLY | O_CLOEXEC);
    if (feed < 0) { std::fprintf(stderr, "sensor-iio: feed open failed: %s\n", strerror(errno)); return 1; }
    ABinderProcess_setThreadPoolMaxThreadCount(4); ABinderProcess_startThreadPool();
    AIBinder* raw = AServiceManager_checkService("android.hardware.sensors.ISensors/default");
    auto sensors = ISensors::fromBinder(ndk::SpAIBinder(raw));
    if (!sensors) { std::fprintf(stderr, "sensor-iio: HAL service unavailable\n"); close(feed); return 1; }
    std::vector<SensorInfo> list;
    auto result = sensors->getSensorsList(&list);
    if (!result.isOk()) { std::fprintf(stderr, "sensor-iio: sensor enumeration failed\n"); close(feed); return 1; }
    struct Selected { int32_t handle = -1; SensorInfo info{}; uint32_t kind = 0; } selected[4];
    const int32_t wanted[] = {static_cast<int32_t>(SensorType::ACCELEROMETER),
                              static_cast<int32_t>(SensorType::GYROSCOPE),
                              static_cast<int32_t>(SensorType::LIGHT),
                              static_cast<int32_t>(SensorType::STEP_DETECTOR)};
    const uint32_t kinds[] = {TB330FU_SENSOR_ACCEL, TB330FU_SENSOR_GYRO,
                              TB330FU_SENSOR_LIGHT, TB330FU_SENSOR_STEPS};
    for (unsigned int i = 0; i < 4; ++i) {
        selected[i].kind = kinds[i];
        for (const auto& info : list) if (static_cast<int32_t>(info.type) == wanted[i]) {
            selected[i].handle = info.sensorHandle; selected[i].info = info; break;
        }
        if (selected[i].handle < 0) {
            std::fprintf(stderr, "sensor-iio: required standardized type %d missing\n", wanted[i]);
            close(feed); return 1;
        }
        std::fprintf(stderr, "sensor-iio: selected type=%d handle=%d name=%s\n",
                     wanted[i], selected[i].handle, selected[i].info.name.c_str());
    }
    EventQueue events(512, true); WakeQueue wake(32, true);
    EventFlag* event_flag = nullptr; EventFlag* wake_flag = nullptr;
    if (EventFlag::createEventFlag(events.getEventFlagWord(), &event_flag) != android::NO_ERROR ||
        EventFlag::createEventFlag(wake.getEventFlagWord(), &wake_flag) != android::NO_ERROR) {
        close(feed); return 1;
    }
    auto callback = ndk::SharedRefBase::make<Callback>();
    result = sensors->initialize(events.dupeDesc(), wake.dupeDesc(), callback);
    if (!result.isOk()) { std::fprintf(stderr, "sensor-iio: FMQ initialize failed\n"); close(feed); return 1; }
    for (const auto& s : selected) {
        int64_t period = s.kind == TB330FU_SENSOR_LIGHT ? period_for(s.info, 100000000LL) :
                         s.kind == TB330FU_SENSOR_STEPS ? period_for(s.info, 0) :
                         period_for(s.info, 40000000LL);
        if (!sensors->batch(s.handle, period, 0).isOk() || !sensors->activate(s.handle, true).isOk()) {
            std::fprintf(stderr, "sensor-iio: activation failed for handle=%d\n", s.handle); return 1;
        }
    }
    std::fprintf(stderr, "sensor-iio: HAL/FMQ ready\n");
    __u64 steps = 0;
    while (!stopping.load()) {
        size_t n = events.availableToRead();
        if (!n) { uint32_t state = 0; event_flag->wait(ISensors::EVENT_QUEUE_FLAG_BITS_READ_AND_PROCESS, &state, 1000000000LL, true); n = events.availableToRead(); }
        n = std::min<size_t>(n, 128); if (!n) continue;
        std::vector<Event> buffer(n); if (!events.read(buffer.data(), n)) continue;
        event_flag->wake(ISensors::EVENT_QUEUE_FLAG_BITS_EVENTS_READ);
        while (wake.availableToRead() > 0) { int32_t count = 0; if (!wake.read(&count, 1)) break; if (wake.write(&count, 1)) wake_flag->wake(ISensors::WAKE_LOCK_QUEUE_FLAG_BITS_DATA_WRITTEN); }
        for (const auto& event : buffer) for (const auto& s : selected) if (event.sensorHandle == s.handle) {
            if (s.kind == TB330FU_SENSOR_ACCEL || s.kind == TB330FU_SENSOR_GYRO) {
                if (event.payload.getTag() == Event::EventPayload::Tag::vec3) send_sample(feed, s.kind, event.timestamp, event);
            } else if (s.kind == TB330FU_SENSOR_LIGHT) {
                if (event.payload.getTag() == Event::EventPayload::Tag::scalar) send_sample(feed, s.kind, event.timestamp, event);
            } else if (event.payload.getTag() == Event::EventPayload::Tag::scalar) {
                ++steps; tb330fu_sensor_sample sample{}; sample.version = TB330FU_SENSOR_ABI_VERSION; sample.kind = TB330FU_SENSOR_STEPS; sample.timestamp_ns = event.timestamp > 0 ? event.timestamp : 1; sample.steps = steps; if (::write(feed, &sample, sizeof(sample)) != static_cast<ssize_t>(sizeof(sample))) stopping.store(true);
            }
        }
    }
    for (const auto& s : selected) sensors->activate(s.handle, false);
    EventFlag::deleteEventFlag(&wake_flag); EventFlag::deleteEventFlag(&event_flag); close(feed);
    std::fprintf(stderr, "sensor-iio: clean shutdown\n"); return 0;
}
