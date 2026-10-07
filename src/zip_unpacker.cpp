#include "zip_unpacker.hpp"
#include <fstream>
#include <iostream>
#include <filesystem>
#include <cstring>
#include <stdexcept>
#include <algorithm>

namespace fs = std::filesystem;

namespace pi5_agent {

// =============================================================================
// NATIVE INFLATE (RFC 1951 DEFLATE DECOMPRESSION ENGINE)
// =============================================================================
namespace inflate_engine {

struct BitStream {
    const uint8_t* data;
    size_t size;
    size_t bit_pos;

    BitStream(const uint8_t* d, size_t s) : data(d), size(s), bit_pos(0) {}

    uint32_t read_bits(int count) {
        uint32_t res = 0;
        for (int i = 0; i < count; ++i) {
            size_t byte_idx = bit_pos >> 3;
            if (byte_idx >= size) return 0;
            int bit_idx = bit_pos & 7;
            uint8_t bit = (data[byte_idx] >> bit_idx) & 1;
            res |= (bit << i);
            bit_pos++;
        }
        return res;
    }

    void align_byte() {
        bit_pos = (bit_pos + 7) & ~7ULL;
    }
};

struct HuffmanTree {
    struct Node {
        int symbol = -1;
        int left = -1;
        int right = -1;
    };
    std::vector<Node> nodes;

    void build(const std::vector<int>& code_lengths) {
        nodes.clear();
        nodes.push_back(Node()); // root at 0

        int max_len = 0;
        for (int l : code_lengths) {
            if (l > max_len) max_len = l;
        }
        if (max_len == 0) return;

        std::vector<int> bl_count(max_len + 1, 0);
        for (int l : code_lengths) {
            if (l > 0) bl_count[l]++;
        }

        std::vector<int> next_code(max_len + 1, 0);
        int code = 0;
        for (int bits = 1; bits <= max_len; ++bits) {
            code = (code + bl_count[bits - 1]) << 1;
            next_code[bits] = code;
        }

        for (size_t sym = 0; sym < code_lengths.size(); ++sym) {
            int len = code_lengths[sym];
            if (len == 0) continue;
            int c = next_code[len]++;
            
            // Insert into prefix tree
            int curr = 0;
            for (int bit = len - 1; bit >= 0; --bit) {
                int b = (c >> bit) & 1;
                if (b == 0) {
                    if (nodes[curr].left == -1) {
                        nodes[curr].left = (int)nodes.size();
                        nodes.push_back(Node());
                    }
                    curr = nodes[curr].left;
                } else {
                    if (nodes[curr].right == -1) {
                        nodes[curr].right = (int)nodes.size();
                        nodes.push_back(Node());
                    }
                    curr = nodes[curr].right;
                }
            }
            nodes[curr].symbol = (int)sym;
        }
    }

    int decode(BitStream& bs) const {
        if (nodes.empty()) return -1;
        int curr = 0;
        while (curr >= 0 && curr < (int)nodes.size() && nodes[curr].symbol == -1) {
            uint32_t b = bs.read_bits(1);
            curr = (b == 0) ? nodes[curr].left : nodes[curr].right;
        }
        if (curr >= 0 && curr < (int)nodes.size()) {
            return nodes[curr].symbol;
        }
        return -1;
    }
};

static bool inflate_raw(const uint8_t* comp_data, size_t comp_size, std::vector<uint8_t>& out_data, size_t expected_size) {
    out_data.reserve(expected_size);
    BitStream bs(comp_data, comp_size);

    static const int len_base[] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const int len_extra[] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const int dist_base[] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const int dist_extra[] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    static const int cl_order[] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

    int bfinal = 0;
    while (!bfinal) {
        bfinal = bs.read_bits(1);
        int btype = bs.read_bits(2);

        if (btype == 0) {
            // Uncompressed block
            bs.align_byte();
            uint16_t len = bs.read_bits(16);
            uint16_t nlen = bs.read_bits(16);
            (void)nlen;
            for (int i = 0; i < len; ++i) {
                out_data.push_back((uint8_t)bs.read_bits(8));
            }
        } else if (btype == 1 || btype == 2) {
            HuffmanTree lit_tree;
            HuffmanTree dist_tree;

            if (btype == 1) {
                // Fixed Huffman
                std::vector<int> lit_lens(288, 8);
                for (int i = 144; i <= 255; ++i) lit_lens[i] = 9;
                for (int i = 256; i <= 279; ++i) lit_lens[i] = 7;
                for (int i = 280; i <= 287; ++i) lit_lens[i] = 8;
                lit_tree.build(lit_lens);

                std::vector<int> dist_lens(32, 5);
                dist_tree.build(dist_lens);
            } else {
                // Dynamic Huffman
                int hlit = bs.read_bits(5) + 257;
                int hdist = bs.read_bits(5) + 1;
                int hclen = bs.read_bits(4) + 4;

                std::vector<int> cl_lens(19, 0);
                for (int i = 0; i < hclen; ++i) {
                    cl_lens[cl_order[i]] = bs.read_bits(3);
                }
                HuffmanTree cl_tree;
                cl_tree.build(cl_lens);

                std::vector<int> lit_dist_lens;
                while ((int)lit_dist_lens.size() < hlit + hdist) {
                    int sym = cl_tree.decode(bs);
                    if (sym < 16) {
                        lit_dist_lens.push_back(sym);
                    } else if (sym == 16) {
                        int rep = bs.read_bits(2) + 3;
                        int prev = lit_dist_lens.empty() ? 0 : lit_dist_lens.back();
                        for (int k = 0; k < rep; ++k) lit_dist_lens.push_back(prev);
                    } else if (sym == 17) {
                        int rep = bs.read_bits(3) + 3;
                        for (int k = 0; k < rep; ++k) lit_dist_lens.push_back(0);
                    } else if (sym == 18) {
                        int rep = bs.read_bits(7) + 11;
                        for (int k = 0; k < rep; ++k) lit_dist_lens.push_back(0);
                    }
                }

                std::vector<int> lit_lens(lit_dist_lens.begin(), lit_dist_lens.begin() + hlit);
                std::vector<int> dist_lens(lit_dist_lens.begin() + hlit, lit_dist_lens.end());
                lit_tree.build(lit_lens);
                dist_tree.build(dist_lens);
            }

            while (true) {
                int sym = lit_tree.decode(bs);
                if (sym < 0 || sym > 285) return false;
                if (sym < 256) {
                    out_data.push_back((uint8_t)sym);
                } else if (sym == 256) {
                    break; // End of block
                } else {
                    int len_idx = sym - 257;
                    int length = len_base[len_idx] + bs.read_bits(len_extra[len_idx]);
                    int dist_sym = dist_tree.decode(bs);
                    if (dist_sym < 0 || dist_sym >= 30) return false;
                    int dist = dist_base[dist_sym] + bs.read_bits(dist_extra[dist_sym]);

                    if (dist > (int)out_data.size()) return false;
                    size_t start = out_data.size() - dist;
                    for (int k = 0; k < length; ++k) {
                        out_data.push_back(out_data[start + k]);
                    }
                }
            }
        } else {
            return false; // Reserved / Error
        }
    }
    return true;
}

} // namespace inflate_engine

// =============================================================================
// ZIP PARSER & FILE EXTRACTION IMPLEMENTATION
// =============================================================================

#pragma pack(push, 1)
struct ZipLocalHeader {
    uint32_t signature;       // 0x04034b50
    uint16_t version_needed;
    uint16_t flags;
    uint16_t compression_method;
    uint16_t last_mod_time;
    uint16_t last_mod_date;
    uint32_t crc32;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint16_t filename_length;
    uint16_t extra_field_length;
};
#pragma pack(pop)

bool ZipUnpacker::extract_file(const std::string& zip_path, const std::string& output_dir) {
    std::ifstream file(zip_path, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "[ZipUnpacker] Không thể mở file: " << zip_path << std::endl;
        return false;
    }
    file.seekg(0, std::ios::end);
    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer(size);
    file.read(reinterpret_cast<char*>(buffer.data()), size);
    return extract_buffer(buffer, output_dir);
}

bool ZipUnpacker::extract_buffer(const std::vector<uint8_t>& zip_data, const std::string& output_dir) {
    if (zip_data.size() < sizeof(ZipLocalHeader)) {
        return false;
    }

    try {
        fs::create_directories(output_dir);
    } catch (const std::exception& e) {
        std::cerr << "[ZipUnpacker] Lỗi tạo thư mục " << output_dir << ": " << e.what() << std::endl;
        return false;
    }

#ifndef _WIN32
    // Thử sử dụng lệnh unzip hoặc python3 trên Linux (nhanh, chuẩn và hỗ trợ mọi định dạng ZIP Data Descriptor)
    static std::atomic<uint64_t> s_zip_cnt{0};
    uint64_t zid = ++s_zip_cnt;
    fs::path temp_zip_path = fs::path(output_dir) / ("temp_pkg_" + std::to_string(zid) + ".zip");

    {
        std::ofstream zf(temp_zip_path, std::ios::binary);
        if (zf.is_open()) {
            zf.write(reinterpret_cast<const char*>(zip_data.data()), zip_data.size());
            zf.close();
        }
    }

    if (fs::exists(temp_zip_path)) {
        std::string cmd = "unzip -q -o \"" + temp_zip_path.string() + "\" -d \"" + output_dir + "\" >/dev/null 2>&1";
        int ret = std::system(cmd.c_str());
        if (ret == 0) {
            try { fs::remove(temp_zip_path); } catch (...) {}
            return true;
        }

        std::string py_cmd = "python3 -m zipfile -e \"" + temp_zip_path.string() + "\" \"" + output_dir + "\" >/dev/null 2>&1";
        ret = std::system(py_cmd.c_str());
        if (ret == 0) {
            try { fs::remove(temp_zip_path); } catch (...) {}
            return true;
        }
        try { fs::remove(temp_zip_path); } catch (...) {}
    }
#endif

    size_t offset = 0;
    while (offset + sizeof(ZipLocalHeader) <= zip_data.size()) {
        const auto* hdr = reinterpret_cast<const ZipLocalHeader*>(&zip_data[offset]);
        if (hdr->signature != 0x04034b50) {
            // Đã hết Local File Headers hoặc chạm tới Central Directory
            break;
        }

        size_t name_offset = offset + sizeof(ZipLocalHeader);
        if (name_offset + hdr->filename_length > zip_data.size()) {
            std::cerr << "[ZipUnpacker] Lỗi vượt kích thước filename header!" << std::endl;
            return false;
        }

        std::string filename(reinterpret_cast<const char*>(&zip_data[name_offset]), hdr->filename_length);
        std::replace(filename.begin(), filename.end(), '\\', '/');
        size_t data_offset = name_offset + hdr->filename_length + hdr->extra_field_length;

        bool is_dir = filename.empty() || filename.back() == '/';
        fs::path target_path = fs::path(output_dir) / filename;

        if (is_dir) {
            try {
                fs::create_directories(target_path);
            } catch (...) {}
            offset = data_offset + hdr->compressed_size;
            continue;
        }

        if (target_path.has_parent_path()) {
            try {
                fs::create_directories(target_path.parent_path());
            } catch (...) {}
        }

        if (data_offset + hdr->compressed_size > zip_data.size()) {
            std::cerr << "[ZipUnpacker] Lỗi payload file bị cắt ngắn!" << std::endl;
            return false;
        }

        std::vector<uint8_t> decompressed_data;
        if (hdr->compression_method == 0) {
            // Uncompressed / Stored
            decompressed_data.assign(
                zip_data.begin() + data_offset,
                zip_data.begin() + data_offset + hdr->uncompressed_size
            );
        } else if (hdr->compression_method == 8) {
            // Deflated
            bool ok = inflate_engine::inflate_raw(
                &zip_data[data_offset],
                hdr->compressed_size,
                decompressed_data,
                hdr->uncompressed_size
            );
            if (!ok) {
                std::cerr << "[ZipUnpacker] Lỗi giải nén Deflate cho file: " << filename << std::endl;
                return false;
            }
        } else {
            std::cerr << "[ZipUnpacker] Phương thức nén không hỗ trợ: " << hdr->compression_method << std::endl;
            return false;
        }

        std::ofstream out_file(target_path, std::ios::binary);
        if (!out_file.is_open()) {
            std::cerr << "[ZipUnpacker] Không thể ghi file: " << target_path << std::endl;
            return false;
        }
        out_file.write(reinterpret_cast<const char*>(decompressed_data.data()), decompressed_data.size());
        out_file.close();

        offset = data_offset + hdr->compressed_size;
    }

    return true;
}

std::vector<ZipFileInfo> ZipUnpacker::list_contents(const std::vector<uint8_t>& zip_data) {
    std::vector<ZipFileInfo> list;
    size_t offset = 0;
    while (offset + sizeof(ZipLocalHeader) <= zip_data.size()) {
        const auto* hdr = reinterpret_cast<const ZipLocalHeader*>(&zip_data[offset]);
        if (hdr->signature != 0x04034b50) break;

        size_t name_offset = offset + sizeof(ZipLocalHeader);
        if (name_offset + hdr->filename_length > zip_data.size()) break;

        std::string filename(reinterpret_cast<const char*>(&zip_data[name_offset]), hdr->filename_length);
        ZipFileInfo info;
        info.filename = filename;
        info.uncompressed_size = hdr->uncompressed_size;
        info.compressed_size = hdr->compressed_size;
        info.is_directory = (!filename.empty() && (filename.back() == '/' || filename.back() == '\\'));
        list.push_back(info);

        size_t data_offset = name_offset + hdr->filename_length + hdr->extra_field_length;
        offset = data_offset + hdr->compressed_size;
    }
    return list;
}

} // namespace pi5_agent
