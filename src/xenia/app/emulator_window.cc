/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/emulator_window.h"

// Autogenerated by `xb premake`.
#include "build/version.h"
#include "third_party/imgui/imgui.h"
#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/debugging.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/base/profiling.h"
#include "xenia/base/threading.h"
#include "xenia/emulator.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/ui/file_picker.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"

#include <fstream>
#include <string>

DECLARE_bool(debug);
DEFINE_int32(window_height, 720, "Window height", "UI");
DEFINE_int32(window_width, 1280, "Window width", "UI");

namespace xe {
namespace app {

using xe::ui::FileDropEvent;
using xe::ui::KeyEvent;
using xe::ui::MenuItem;
using xe::ui::MouseEvent;
using xe::ui::UIEvent;

const std::wstring kBaseTitle = L"xenia-canary";

ui::NativeWindowHandle window_handle;

EmulatorWindow::EmulatorWindow(Emulator* emulator)
    : emulator_(emulator),
      loop_(ui::Loop::Create()),
      window_(ui::Window::Create(loop_.get(), kBaseTitle)) {
  base_title_ = kBaseTitle +
#ifdef DEBUG
#if _NO_DEBUG_HEAP == 1
                L" DEBUG" +
#else
                L" CHECKED" +
#endif
#endif
                L" (" + xe::to_wstring(XE_BUILD_BRANCH) + L"/" +
                L"0.0.0." +
                xe::to_wstring(XE_BUILD_COMMIT_NUMBER) + L"/" +
                xe::to_wstring(XE_BUILD_COMMIT_SHORT) + L"/" +
                xe::to_wstring(XE_BUILD_DATE) + L")";
}

EmulatorWindow::~EmulatorWindow() {
  loop_->PostSynchronous([this]() { window_.reset(); });
}

std::unique_ptr<EmulatorWindow> EmulatorWindow::Create(Emulator* emulator) {
  std::unique_ptr<EmulatorWindow> emulator_window(new EmulatorWindow(emulator));

  emulator_window->loop()->PostSynchronous([&emulator_window]() {
    xe::threading::set_name("Win32 Loop");
    xe::Profiler::ThreadEnter("Win32 Loop");

    if (!emulator_window->Initialize()) {
      xe::FatalError("Failed to initialize main window");
      return;
    }
  });

  return emulator_window;
}

bool EmulatorWindow::Initialize() {
  if (!window_->Initialize()) {
    XELOGE("Failed to initialize platform window");
    return false;
  }

  UpdateTitle();

  window_->on_closed.AddListener([this](UIEvent* e) { loop_->Quit(); });
  loop_->on_quit.AddListener([this](UIEvent* e) { window_.reset(); });

  window_->on_file_drop.AddListener(
      [this](FileDropEvent* e) { FileDrop(e->filename()); });

  window_->on_key_down.AddListener([this](KeyEvent* e) {
    bool handled = true;
    switch (e->key_code()) {
      case 0x48: {  // h
        if (e->is_shift_pressed()) {
          window_->FPSTextScale();
        } else {
          window_->ToggleFPS();
        }
      } break;
      case 0x4F: {  // o
        if (e->is_ctrl_pressed()) {
          FileOpen();
        }
      } break;
      case 0x6A: {  // numpad *
        CpuTimeScalarReset();
      } break;
      case 0x6D: {  // numpad minus
        CpuTimeScalarSetHalf();
      } break;
      case 0x6B: {  // numpad plus
        CpuTimeScalarSetDouble();
      } break;

      case 0x72: {  // F3
        Profiler::ToggleDisplay();
      } break;

      case 0x73: {  // VK_F4
        GpuTraceFrame();
      } break;
      case 0x74: {  // VK_F5
        GpuClearCaches();
      } break;
      case 0x76: {  // VK_F7
        // Save to file
        // TODO: Choose path based on user input, or from options
        // TODO: Spawn a new thread to do this.
        emulator()->SaveToFile(L"test.sav");
      } break;
      case 0x77: {  // VK_F8
        // Restore from file
        // TODO: Choose path from user
        // TODO: Spawn a new thread to do this.
        emulator()->RestoreFromFile(L"test.sav");
      } break;
      case 0x7A: {  // VK_F11
        ToggleFullscreen();
      } break;
      case 0x1B: {  // VK_ESCAPE
                    // Allow users to escape fullscreen (but not enter it).
        if (window_->is_fullscreen()) {
          window_->ToggleFullscreen(false);
        } else {
          handled = false;
        }
      } break;

      case 0x13: {  // VK_PAUSE
        CpuBreakIntoDebugger();
      } break;
      case 0x03: {  // VK_CANCEL
        CpuBreakIntoHostDebugger();
      } break;

      case 0x70: {  // VK_F1
        ShowHelpWebsite();
      } break;
	  
      case 0x71: {  // VK_F2
        ShowCommitID();
      } break;

      default: {
        handled = false;
      } break;
    }
    e->set_handled(handled);
  });

  window_->on_mouse_move.AddListener([this](MouseEvent* e) {
    if (window_->is_fullscreen() && (e->dx() > 2 || e->dy() > 2)) {
      if (!window_->is_cursor_visible()) {
        window_->set_cursor_visible(true);
      }

      cursor_hide_time_ = Clock::QueryHostSystemTime() + 30000000;
    }

    e->set_handled(false);
  });

  window_->on_paint.AddListener([this](UIEvent* e) { CheckHideCursor(); });

  // Main menu.
  // FIXME: This code is really messy.
  auto main_menu = MenuItem::Create(MenuItem::Type::kNormal);
  auto file_menu = MenuItem::Create(MenuItem::Type::kPopup, L"&File");
  // Recent Sub tab
  auto recent_list = MenuItem::Create(MenuItem::Type::kPopup, L"&Open Recent");
  //******Add Recent PATHS to menu***************************************
  std::string lines[10];
  std::ifstream recent_file_read("recent.txt");
  int i = 0;
  // Convert from UTF-8 to wide using helper function xe::to_wstring()
  if (recent_file_read.is_open()) {
    while ((i < 10) && getline(recent_file_read, lines[i])) {
      // Save paths to memory
      global_recent_paths_[i] = xe::to_wstring(lines[i]);
      // Add most recently opened files to menu
      if (lines[i] != "" && lines[i] != "\n") {
        recent_list->AddChild(
            MenuItem::Create(MenuItem::Type::kString, global_recent_paths_[i],
                             std::bind(&EmulatorWindow::RecentList, this, i)));
      } else {
        break;
      }
      i++;
    }
    recent_file_read.close();
  }
  //****************************************************************************
  {
    file_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, L"&Open...", L"Ctrl+O",
                         std::bind(&EmulatorWindow::FileOpen, this)));
    // Add Recent List Tab to file tab
    file_menu->AddChild(std::move(recent_list));

    file_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, L"Close",
                         std::bind(&EmulatorWindow::FileClose, this)));
    file_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
    file_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, L"Show content directory...",
        std::bind(&EmulatorWindow::ShowContentDirectory, this)));
    file_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
    file_menu->AddChild(MenuItem::Create(MenuItem::Type::kString, L"E&xit",
                                         L"Alt+F4",
                                         [this]() { window_->Close(); }));
  }
  main_menu->AddChild(std::move(file_menu));

  // CPU menu.
  auto cpu_menu = MenuItem::Create(MenuItem::Type::kPopup, L"&CPU");
  {
    cpu_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, L"&Reset Time Scalar", L"Numpad *",
        std::bind(&EmulatorWindow::CpuTimeScalarReset, this)));
    cpu_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, L"Time Scalar /= 2", L"Numpad -",
        std::bind(&EmulatorWindow::CpuTimeScalarSetHalf, this)));
    cpu_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, L"Time Scalar *= 2", L"Numpad +",
        std::bind(&EmulatorWindow::CpuTimeScalarSetDouble, this)));
  }
  cpu_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
  {
    cpu_menu->AddChild(MenuItem::Create(MenuItem::Type::kString,
                                        L"Toggle Profiler &Display", L"F3",
                                        []() { Profiler::ToggleDisplay(); }));
    cpu_menu->AddChild(MenuItem::Create(MenuItem::Type::kString,
                                        L"&Pause/Resume Profiler", L"`",
                                        []() { Profiler::TogglePause(); }));
  }
  cpu_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
  {
    cpu_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, L"&Break and Show Guest Debugger",
        L"Pause/Break",
        std::bind(&EmulatorWindow::CpuBreakIntoDebugger, this)));
    cpu_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, L"&Break into Host Debugger",
        L"Ctrl+Pause/Break",
        std::bind(&EmulatorWindow::CpuBreakIntoHostDebugger, this)));
  }
  main_menu->AddChild(std::move(cpu_menu));

  // GPU menu.
  auto gpu_menu = MenuItem::Create(MenuItem::Type::kPopup, L"&GPU");
  {
    gpu_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, L"&Trace Frame", L"F4",
                         std::bind(&EmulatorWindow::GpuTraceFrame, this)));
  }
  gpu_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
  {
    gpu_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, L"&Clear Caches", L"F5",
                         std::bind(&EmulatorWindow::GpuClearCaches, this)));
  }
  main_menu->AddChild(std::move(gpu_menu));

  // Window menu.
  auto window_menu = MenuItem::Create(MenuItem::Type::kPopup, L"&Window");
  {
    window_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, L"&Fullscreen", L"F11",
                         std::bind(&EmulatorWindow::ToggleFullscreen, this)));
  }
  main_menu->AddChild(std::move(window_menu));

  // Help menu.
  auto help_menu = MenuItem::Create(MenuItem::Type::kPopup, L"&Help");
  {
    help_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, L"Build commit on GitHub...", L"F2",
        std::bind(&EmulatorWindow::ShowCommitID, this)));
    help_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, L"Recent changes on GitHub...", [this]() {
          std::wstring url =
              std::wstring(L"https://github.com/xenia-canary/xenia-canary/compare/") +
              xe::to_wstring(XE_BUILD_COMMIT) + L"..." +
              xe::to_wstring(XE_BUILD_BRANCH);
          LaunchBrowser(url.c_str());
        }));
    help_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
    help_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, L"&Website...", L"F1",
                         std::bind(&EmulatorWindow::ShowHelpWebsite, this)));
    help_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, L"&About...",
        [this]() { LaunchBrowser(L"https://xenia.jp/about/"); }));
  }
  main_menu->AddChild(std::move(help_menu));

  window_->set_main_menu(std::move(main_menu));

  window_->Resize(cvars::window_width, cvars::window_height);

  window_->DisableMainMenu();

  return true;
}

void EmulatorWindow::FileDrop(const wchar_t* filename) {
  auto result = emulator_->LaunchPath(filename);
  if (XFAILED(result)) {
    // TODO: Display a message box.
    XELOGE("Failed to launch target: %.8X", result);
  }
}

// Add new path to recent storage or prioritize if it already exist
void EmulatorWindow::RecentListUpdater(std::wstring path) {
  //*******Save abs_path to recent as UTF-8******************************
  std::string u8_path = xe::to_string(path);

  // Write to 'recent.txt' file
  std::string lines[10];
  int i = 0;
  int path_poss = 0;
  bool path_is_in = false;
  // Open file "recent.txt" to read values
  std::ifstream recent_file_read("recent.txt");
  if (recent_file_read.is_open()) {
    // Loop until end of document or 10 lines are read, which ever comes first
    while (getline(recent_file_read, lines[i]) && (i < 9)) {
      if (u8_path.compare(lines[i]) == 0) {
        path_is_in = true;
        path_poss = i;
      }
      i++;
    }
    recent_file_read.close();
  }
  // Move lines
  if (!path_is_in) {
    path_poss = 9;
  }
  for (i = path_poss; i > 0; i--) {
    lines[i] = lines[i - 1];
  }
  // Add all lines to file
  lines[0] = u8_path;
  std::ofstream recent_file_write("recent.txt");
  for (i = 0; i < 10; i++) {
    if (lines[i] != "" && lines[i] != "\n") {
      recent_file_write << lines[i] << std::endl;
    }
  }
  recent_file_write.close();
  //*****************************************************
}

void EmulatorWindow::FileOpen() {
  std::wstring path;

  auto file_picker = xe::ui::FilePicker::Create();
  file_picker->set_mode(ui::FilePicker::Mode::kOpen);
  file_picker->set_type(ui::FilePicker::Type::kFile);
  file_picker->set_multi_selection(false);
  file_picker->set_title(L"Select Content Package");
  file_picker->set_extensions({
      {L"Supported Files", L"*.iso;*.xex;*.xcp;*.*"},
      {L"Disc Image (*.iso)", L"*.iso"},
      {L"Xbox Executable (*.xex)", L"*.xex"},
      //{ L"Content Package (*.xcp)", L"*.xcp" },
      {L"All Files (*.*)", L"*.*"},
  });
  if (file_picker->Show(window_->native_handle())) {
    window_handle = window_->native_handle();
    auto selected_files = file_picker->selected_files();
    if (!selected_files.empty()) {
      path = selected_files[0];
    }
  }

  if (!path.empty()) {
    // Normalize the path and make absolute.
    std::wstring abs_path = xe::to_absolute_path(path);

    EmulatorWindow::RecentListUpdater(abs_path);

    auto result = emulator_->LaunchPath(abs_path);
    if (XFAILED(result)) {
      // TODO: Display a message box.
      XELOGE("Failed to launch target: %.8X", result);
    }
  }
}

// Launch recently opened files
void EmulatorWindow::RecentList(int index) {
  if (!global_recent_paths_[index].empty()) {
    EmulatorWindow::RecentListUpdater(global_recent_paths_[index]);

    auto result = emulator_->LaunchPath(global_recent_paths_[index]);
    if (XFAILED(result)) {
      // TODO: Display a message box.
      XELOGE("Failed to launch target: %.8X", result);
    }
  }
}

std::wstring EmulatorWindow::SwapNext(int8_t disc_number) {
  std::wstring path = L"";

  auto file_picker = xe::ui::FilePicker::Create();
  file_picker->set_mode(ui::FilePicker::Mode::kOpen);
  file_picker->set_type(ui::FilePicker::Type::kFile);
  file_picker->set_multi_selection(false);
  char titlebar[16];
  sprintf(titlebar, "Select Disc:(%d)", disc_number);
  file_picker->set_title(xe::to_wstring(titlebar));
  file_picker->set_extensions({
      {L"Supported Files", L"*.iso;*.xex;*.xcp;*.*"},
      {L"Disc Image (*.iso)", L"*.iso"},
      {L"Xbox Executable (*.xex)", L"*.xex"},
      //  { L"Content Package (*.xcp)", L"*.xcp" },
      {L"All Files (*.*)", L"*.*"},
  });
  if (file_picker->Show(window_handle)) {
    auto selected_files = file_picker->selected_files();
    if (!selected_files.empty()) {
      path = selected_files[0];
    }
  }
  if (path != L"") {
    // Normalize the path and make absolute.
    std::wstring abs_path = xe::to_absolute_path(path);
  }
  XELOGD("SwapNext Return Path:(%S)", path.c_str());
  return path;
}

void EmulatorWindow::FileClose() {
  if (emulator_->is_title_open()) {
    emulator_->TerminateTitle();
  }
}

void EmulatorWindow::ShowContentDirectory() {
  std::wstring target_path;

  auto content_root = emulator_->content_root();
  if (!emulator_->is_title_open() || !emulator_->kernel_state()) {
    target_path = content_root;
  } else {
    // TODO(gibbed): expose this via ContentManager?
    wchar_t title_id[9] = L"00000000";
    std::swprintf(title_id, 9, L"%.8X", emulator_->kernel_state()->title_id());
    auto package_root = xe::join_paths(content_root, title_id);
    target_path = package_root;
  }

  if (!xe::filesystem::PathExists(target_path)) {
    xe::filesystem::CreateFolder(target_path);
  }

  LaunchBrowser(target_path.c_str());
}

void EmulatorWindow::CheckHideCursor() {
  if (!window_->is_fullscreen()) {
    // Only hide when fullscreen.
    return;
  }

  if (Clock::QueryHostSystemTime() > cursor_hide_time_) {
    window_->set_cursor_visible(false);
  }
}

void EmulatorWindow::CpuTimeScalarReset() {
  Clock::set_guest_time_scalar(1.0);
  UpdateTitle();
}

void EmulatorWindow::CpuTimeScalarSetHalf() {
  Clock::set_guest_time_scalar(Clock::guest_time_scalar() / 2.0);
  UpdateTitle();
}

void EmulatorWindow::CpuTimeScalarSetDouble() {
  Clock::set_guest_time_scalar(Clock::guest_time_scalar() * 2.0);
  UpdateTitle();
}

void EmulatorWindow::CpuBreakIntoDebugger() {
  if (!cvars::debug) {
    xe::ui::ImGuiDialog::ShowMessageBox(window_.get(), "Xenia Debugger",
                                        "Xenia must be launched with the "
                                        "--debug flag in order to enable "
                                        "debugging.");
    return;
  }
  auto processor = emulator()->processor();
  if (processor->execution_state() == cpu::ExecutionState::kRunning) {
    // Currently running, so interrupt (and show the debugger).
    processor->Pause();
  } else {
    // Not running, so just bring the debugger into focus.
    processor->ShowDebugger();
  }
}

void EmulatorWindow::CpuBreakIntoHostDebugger() { xe::debugging::Break(); }

void EmulatorWindow::GpuTraceFrame() {
  emulator()->graphics_system()->RequestFrameTrace();
}

void EmulatorWindow::GpuClearCaches() {
  emulator()->graphics_system()->ClearCaches();
}

void EmulatorWindow::ToggleFullscreen() {
  window_->ToggleFullscreen(!window_->is_fullscreen());

  // Hide the cursor after a second if we're going fullscreen
  cursor_hide_time_ = Clock::QueryHostSystemTime() + 30000000;
  if (!window_->is_fullscreen()) {
    window_->set_cursor_visible(true);
  }
}

void EmulatorWindow::ShowHelpWebsite() { LaunchBrowser(L"https://xenia.jp"); }

void EmulatorWindow::ShowCommitID() {
          std::wstring url =
              std::wstring(L"https://github.com/xenia-canary/xenia-canary/commit/") +
              xe::to_wstring(XE_BUILD_COMMIT) + L"/";
          LaunchBrowser(url.c_str());
  }

void EmulatorWindow::UpdateTitle() {
  std::wstring title(base_title_);

  if (emulator()->is_title_open()) {
    auto game_title = emulator()->game_title();
    title += xe::format_string(L" | [%.8X] %s", emulator()->title_id(),
                               game_title.c_str());
  }

  auto graphics_system = emulator()->graphics_system();
  if (graphics_system) {
    auto graphics_name = graphics_system->name();
    title += L" <" + graphics_name + L">";
  }

  if (Clock::guest_time_scalar() != 1.0) {
    title += xe::format_string(L" (@%.2fx)", Clock::guest_time_scalar());
  }

  window_->set_title(title);
}

}  // namespace app
}  // namespace xe
