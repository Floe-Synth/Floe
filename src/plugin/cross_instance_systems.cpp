// Copyright 2018-2024 Sam Windell
// SPDX-License-Identifier: GPL-3.0-or-later

#include "cross_instance_systems.hpp"

#include "foundation/foundation.hpp"
#include "os/misc.hpp"

#include "settings/settings_file.hpp"

CrossInstanceSystems::CrossInstanceSystems()
    : arena(PageAllocator::Instance(), Kb(16))
    , logger(g_log_file)
    , paths(CreateFloePaths(arena))
    , settings(paths)
    , sample_library_server(thread_pool,
                            paths.always_scanned_folders[ToInt(ScanFolderType::Libraries)],
                            error_notifications) {
    folder_settings_listener_id =
        settings.tracking.filesystem_change_listeners.Add([this](ScanFolderType type) {
            switch (type) {
                case ScanFolderType::Presets: preset_listing.scanned_folder.needs_rescan.Store(true); break;
                case ScanFolderType::Libraries: {
                    sample_lib_server::SetExtraScanFolders(
                        sample_library_server,
                        settings.settings.filesystem.extra_libraries_scan_folders);
                    break;
                }
                case ScanFolderType::Count: PanicIfReached();
            }
        });
    thread_pool.Init("Global", {});

    // =========
    bool file_is_new = false;
    auto opt_data = FindAndReadSettingsFile(settings.arena, paths);
    if (!opt_data)
        file_is_new = true;
    else
        settings.settings = *opt_data;
    if (InitialiseSettingsFileData(settings.settings, settings.arena, file_is_new))
        settings.tracking.changed = true;

    ASSERT(settings.settings.gui.window_width != 0);

    sample_lib_server::SetExtraScanFolders(sample_library_server,
                                           settings.settings.filesystem.extra_libraries_scan_folders);
}

CrossInstanceSystems::~CrossInstanceSystems() {
    {
        auto outcome = WriteSettingsFileIfChanged(settings);
        if (outcome.HasError()) logger.ErrorLn("Failed to write settings file: {}", outcome.Error());
    }

    settings.tracking.filesystem_change_listeners.Remove(folder_settings_listener_id);
}
