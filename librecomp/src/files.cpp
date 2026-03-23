#include "files.hpp"

constexpr std::u8string_view backup_suffix = u8".bak";
constexpr std::u8string_view temp_suffix = u8".temp";

namespace {
    std::filesystem::path with_suffix(const std::filesystem::path& filepath, std::u8string_view suffix) {
        std::filesystem::path ret{filepath};
        ret += suffix;
        return ret;
    }

    bool refresh_output_backup(const std::filesystem::path& filepath, const std::filesystem::path& backup_path) {
        std::error_code ec;
        std::filesystem::copy_file(filepath, backup_path, std::filesystem::copy_options::overwrite_existing, ec);
        if (!ec) {
            return true;
        }

#if defined(__ANDROID__)
        ec.clear();
        std::filesystem::remove(backup_path, ec);
        if (ec) {
            return false;
        }

        ec.clear();
        std::filesystem::copy_file(filepath, backup_path, std::filesystem::copy_options::none, ec);
        return !ec;
#else
        return false;
#endif
    }

#if defined(__ANDROID__)
    bool replace_output_with_temp_file(const std::filesystem::path& filepath, const std::filesystem::path& temp_path) {
        std::error_code ec;
        std::filesystem::rename(temp_path, filepath, ec);
        if (!ec) {
            return true;
        }

        ec.clear();
        std::filesystem::remove(filepath, ec);
        if (!ec) {
            ec.clear();
            std::filesystem::rename(temp_path, filepath, ec);
            if (!ec) {
                return true;
            }
        }

        ec.clear();
        std::filesystem::copy_file(temp_path, filepath, std::filesystem::copy_options::overwrite_existing, ec);
        if (!ec) {
            std::error_code remove_ec;
            std::filesystem::remove(temp_path, remove_ec);
            return true;
        }

        return false;
    }
#endif

    bool promote_output_temp_file(const std::filesystem::path& filepath, const std::filesystem::path& temp_path) {
        std::error_code ec;
        std::filesystem::copy_file(temp_path, filepath, std::filesystem::copy_options::overwrite_existing, ec);
        if (!ec) {
            std::error_code remove_ec;
            std::filesystem::remove(temp_path, remove_ec);
            return true;
        }

#if defined(__ANDROID__)
        return replace_output_with_temp_file(filepath, temp_path);
#else
        return false;
#endif
    }
}

std::ifstream recomp::open_input_backup_file(const std::filesystem::path& filepath, std::ios_base::openmode mode) {
    std::filesystem::path backup_path = with_suffix(filepath, backup_suffix);
    return std::ifstream{backup_path, mode};
}

std::ifstream recomp::open_input_file_with_backup(const std::filesystem::path& filepath, std::ios_base::openmode mode) {
    std::ifstream ret{filepath, mode};

    // Check if the file failed to open and open the corresponding backup file instead if so.
    if (!ret.good()) {
        return open_input_backup_file(filepath, mode);
    }

    return ret;
}

std::ofstream recomp::open_output_file_with_backup(const std::filesystem::path& filepath, std::ios_base::openmode mode) {
    std::filesystem::path temp_path = with_suffix(filepath, temp_suffix);
    std::ofstream temp_file_out{ temp_path, mode };

#if defined(__ANDROID__)
    if (!temp_file_out.good()) {
        std::error_code ec;
        std::filesystem::remove(temp_path, ec);
        if (!ec) {
            temp_file_out = std::ofstream{ temp_path, mode };
        }
    }
#endif

    return temp_file_out;
}

bool recomp::finalize_output_file_with_backup(const std::filesystem::path& filepath) {
    std::filesystem::path backup_path = with_suffix(filepath, backup_suffix);
    std::filesystem::path temp_path = with_suffix(filepath, temp_suffix);

    std::error_code ec;
    if (std::filesystem::exists(filepath, ec)) {
        if (!refresh_output_backup(filepath, backup_path)) {
#if !defined(__ANDROID__)
            return false;
#endif
        }
    }

    if (!promote_output_temp_file(filepath, temp_path)) {
        return false;
    }

    return true;
}
