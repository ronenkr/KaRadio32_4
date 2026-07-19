/*
 * FileSystem.hpp - rewritten to mount the `littlefs` data partition
 * (defined in partitions.ttgo_tdisplay_s3.csv, shared with KaRadio's
 * partition table) instead of an SD card. WADs live directly under the
 * mount point instead of /sdcard/doom.
 *
 * The directory-listing helpers below are unchanged from the original -
 * they're plain POSIX dirent/stat calls that work identically against any
 * mounted VFS filesystem, LittleFS included.
 */
#ifndef MAIN_FILESYSTEM_HPP_
#define MAIN_FILESYSTEM_HPP_

#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include <string>
#include <cstring>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_littlefs.h>

namespace EraTV
{
	// Mount point used throughout main.cpp for WAD discovery.
	static constexpr const char *kLittleFsMountPoint = "/littlefs";

	class FileSystem
	{
	public:
		FileSystem()
		{
		}

		esp_err_t begin()
		{
			esp_vfs_littlefs_conf_t conf = {
				.base_path = kLittleFsMountPoint,
				.partition_label = "littlefs",
				.format_if_mount_failed = false,
				.dont_mount = false,
			};

			esp_err_t ret = esp_vfs_littlefs_register(&conf);
			if (ret == ESP_OK) {
				size_t total = 0, used = 0;
				esp_littlefs_info(conf.partition_label, &total, &used);
				ESP_LOGI(TAG, "littlefs mounted at %s: %u/%u bytes used",
						 kLittleFsMountPoint, (unsigned)used, (unsigned)total);
				return ret;
			}

			// Unlike the original SD-card path, don't reboot on failure - an
			// empty/unformatted littlefs partition (e.g. before flash_wads.sh
			// has ever been run) is a normal, recoverable first-boot state,
			// not a hardware fault. main.cpp's WAD scan will simply report
			// "no IWAD found" instead.
			ESP_LOGE(TAG, "littlefs mount failed: %s", esp_err_to_name(ret));
			return ret;
		}

		FILE *openfile(std::string &file)
		{
			return fopen(file.c_str(), "rb");
		}

		int closefile(FILE *file)
		{
			return fclose(file);
		}

		std::vector<std::string> list_sd(const std::string &sdir)
		{
			std::vector<std::string> list;

			DIR *dir;
			struct dirent *ent;
			if ((dir = opendir(sdir.c_str())) != NULL)
			{
				while ((ent = readdir(dir)) != NULL)
				{
					list.push_back(ent->d_name);
				}
				closedir(dir);
			}

			return list;
		}

		std::vector<std::string> list_sdcard_recursive(const std::string &path)
		{
			std::vector<std::string> files;
			DIR *dir = opendir(path.c_str());
			if (!dir)
			{
				ESP_LOGE(TAG, "Failed to open directory: %s", path.c_str());
				return files;
			}
			struct dirent *entry;
			while ((entry = readdir(dir)) != NULL)
			{
				if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
					continue;
				std::string full_path = path + "/" + entry->d_name;
				files.push_back(full_path);
				if (entry->d_type == DT_DIR)
				{
					auto sub_files = list_sdcard_recursive(full_path);
					files.insert(files.end(), sub_files.begin(), sub_files.end());
				}
			}
			closedir(dir);
			return files;
		}

		void print_sdcard_recursive(const std::string &path)
		{
			DIR *dir = opendir(path.c_str());
			if (!dir)
			{
				ESP_LOGE(TAG, "Failed to open directory: %s", path.c_str());
				return;
			}
			struct dirent *entry;
			while ((entry = readdir(dir)) != NULL)
			{
				if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
					continue;
				std::string full_path = path + "/" + entry->d_name;
				struct stat st;
				if (stat(full_path.c_str(), &st) == 0)
				{
					if (S_ISDIR(st.st_mode))
					{
						ESP_LOGI(TAG, "DIR : %s", full_path.c_str());
						list_sdcard_recursive(full_path);
					}
					else
					{
						ESP_LOGI(TAG, "FILE: %s", full_path.c_str());
					}
				}
			}
			closedir(dir);
		}

	private:
		const char *TAG = "EraTV::FileSystem";
	};

}

#endif /* MAIN_FILESYSTEM_HPP_ */
