#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace pi5_agent {

struct ZipFileInfo {
    std::string filename;
    uint32_t uncompressed_size = 0;
    uint32_t compressed_size = 0;
    bool is_directory = false;
};

class ZipUnpacker {
public:
    /**
     * @brief Giải nén file ZIP từ đường dẫn file trên đĩa
     * @param zip_path Đường dẫn tới file .zip
     * @param output_dir Thư mục đích cần giải nén
     * @return true nếu giải nén thành công, false nếu lỗi
     */
    static bool extract_file(const std::string& zip_path, const std::string& output_dir);

    /**
     * @brief Giải nén dữ liệu ZIP trực tiếp từ mảng byte trong bộ nhớ (Buffer)
     * @param zip_data Mảng byte dữ liệu zip
     * @param output_dir Thư mục đích cần giải nén
     * @return true nếu giải nén thành công, false nếu lỗi
     */
    static bool extract_buffer(const std::vector<uint8_t>& zip_data, const std::string& output_dir);

    /**
     * @brief Liệt kê danh sách các file có trong gói ZIP mà không giải nén
     */
    static std::vector<ZipFileInfo> list_contents(const std::vector<uint8_t>& zip_data);
};

} // namespace pi5_agent
