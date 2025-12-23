#pragma once

#include "services/IDiskService.hpp"

class DiskService : public IDiskService {
public:
    DiskService() = default;
    ~DiskService() override = default;
    
    // Non-copyable, moveable
    DiskService(const DiskService&) = delete;
    DiskService& operator=(const DiskService&) = delete;
    DiskService(DiskService&&) = default;
    DiskService& operator=(DiskService&&) = default;
    
    [[nodiscard]] auto get_available_disks() -> std::vector<DiskInfo> override;
    auto unmount_disk(const std::string& path) -> std::expected<void, util::Error> override;
    [[nodiscard]] auto is_disk_writable(const std::string& path) -> bool override;
    [[nodiscard]] auto get_disk_size(const std::string& path) -> std::expected<uint64_t, util::Error> override;
    [[nodiscard]] auto validate_device_path(const std::string& path) -> std::expected<void, util::Error> override;

private:
    [[nodiscard]] auto parse_disk_info(const std::string& device_path) -> DiskInfo;
    [[nodiscard]] auto check_if_ssd(const std::string& device_path) -> bool;
};
