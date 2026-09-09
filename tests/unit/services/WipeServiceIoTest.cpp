#include "algorithms/ATASecureEraseAlgorithm.hpp"
#include "fixtures/FakeDeviceIO.hpp"
#include "fixtures/TestFixtures.hpp"
#include "helper/services/WipeService.hpp"
#include "mocks/MockDiskService.hpp"
#include "util/WriteHelpers.hpp"

#include <future>

namespace {

constexpr auto TARGET = "/dev/nvme999n1";
constexpr auto SIBLING = "/dev/nvme999n2";
constexpr auto CONTROLLER = "/dev/nvme999";

class WipeServiceIoTest : public testing::Test {
protected:
    FakeDeviceIO io;
    std::shared_ptr<MockDiskService> disks = MockDiskService::CreateNiceMock();
    std::unique_ptr<WipeService> service;
    std::vector<DiskInfo> disk_list;
    std::vector<WipeProgress> progress;
    std::mutex progress_mutex;

    void SetUp() override {
        io.devices[TARGET] = {};
        io.devices[SIBLING] = {};
        io.devices[CONTROLLER] = {};
        disk_list = {MockDiskService::CreateTestDisk(TARGET, 2'048),
                     MockDiskService::CreateTestDisk(SIBLING, 2'048)};
        ON_CALL(*disks, get_available_disks_blocking()).WillByDefault([this] { return disk_list; });
        service = std::make_unique<WipeService>(disks);
    }

    auto callback() -> ProgressCallback {
        return [this](const WipeProgress& value) {
            std::lock_guard lock(progress_mutex);
            progress.push_back(value);
        };
    }

    void wait_for_completion() {
        ASSERT_TRUE(ThreadingTestHelper::WaitUntil(
            [this] { return !service->is_operation_in_progress(TARGET); }));
        service.reset();  // Join before inspecting simulated I/O or progress.
    }

    auto terminals() const -> size_t {
        return std::ranges::count_if(progress, [](const auto& p) { return p.is_complete; });
    }
};

TEST(WriteHelpersIoTest, ShortWriteThenErrorRetriesAtOriginalBufferOffset) {
    FakeDeviceIO io;
    io.devices[TARGET].bytes.assign(2'048, 0xAB);
    io.devices[TARGET].short_then_error = true;
    const int fd = open(TARGET, O_RDWR);
    ASSERT_GE(fd, 0);
    std::vector<uint8_t> zeros(2'048);
    uint64_t bad = 0;
    EXPECT_EQ(util::write_with_bad_sector_tolerance(fd, zeros, bad), 2'048u);
    EXPECT_EQ(lseek(fd, 0, SEEK_CUR), 2'048);
    EXPECT_EQ(io.devices[TARGET].bytes, zeros);
    EXPECT_EQ(io.devices[TARGET].retry_offsets, (std::vector<off_t>{512, 1'024, 1'536}));
    EXPECT_EQ(bad, 0u);
    close(fd);
}

}  // namespace
