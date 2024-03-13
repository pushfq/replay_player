#include <Windows.h>
#include <ctfp/ctfp.h>
#include <osp/osr/replay_file.h>
#include <winternl.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <print>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

namespace {

enum struct osu_modes : std::int32_t {
  play = 2,
};

constexpr auto update_rate = std::chrono::milliseconds(1);

HANDLE proc = INVALID_HANDLE_VALUE;

std::uint32_t time_pointer = 0x0;
std::uint32_t mode_pointer = 0x0;
std::uint32_t gf_ptr_ptr = 0x0;

std::unique_ptr<osp::osr::replay_file> active_replay;

osp::osr::replay_frames::const_iterator current_frame;

/**
 * @brief Dereference the audio time pointer.
 * @return The current time in milliseconds.
 */
int32_t read_time() {
  int32_t time = 0;
  ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(time_pointer), &time, sizeof(time), nullptr);
  return time;
}

/**
 * @brief Dereference the mode pointer.
 * @return The current game mode.
 */
osu_modes read_mode() {
  osu_modes mode;
  ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(mode_pointer), &mode, sizeof(mode), nullptr);
  return mode;
}

/**
 * @brief Dereference the gamefield pointer.
 * @note This pointer should be treated as volatile.
 * @return The gamefield pointer.
 */
uint32_t read_gamefield() {
  uint32_t gamefield;
  ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(gf_ptr_ptr), &gamefield, sizeof(gamefield), nullptr);
  return gamefield;
}

/**
 * @brief Dereference the window pointer.
 * @return The window pointer.
 */
uint32_t read_window() {
  const auto gamefield = read_gamefield();

  uint32_t window;
  ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(gamefield + 0x4), &window, sizeof(window), nullptr);

  return window;
}

/**
 * @brief Read the window size.
 * @return The window size.
 */
osp::vector2f read_window_size() {
  const auto window = read_window();

  std::uint32_t x;
  std::uint32_t y;

  ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(window + 0x4), &x, sizeof(x), nullptr);
  ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(window + 0x8), &y, sizeof(y), nullptr);

  return osp::vector2f(static_cast<float>(x), static_cast<float>(y));
}

/**
 * @brief Scale a position to the gamefield.
 * @param pos The position to scale.
 * @return The scaled position.
 */
osp::vector2f scale(osp::vector2f pos) {
  constexpr auto playfield_width = 512.0F;
  constexpr auto playfield_height = 384.0F;

  const auto size = read_window_size();

  const auto width = size.x;
  const auto height = size.y;

  const auto ratio = height / 480.0F;
  const auto width_ratio = playfield_width * ratio;
  const auto height_ratio = playfield_height * ratio;

  const auto vx = (width - width_ratio) * 0.5F;
  const auto vy = ((height - height_ratio) / 4.0F) * 3.0F + -16.0F * ratio;

  const auto rate_x = pos.x / playfield_width;
  const auto rate_y = pos.y / playfield_height;

  return osp::vector2f(width_ratio * rate_x + vx, height_ratio * rate_y + vy);
}

/**
 * @brief Open a process by name.
 * @param process_name The name of the process to open.
 * @return True if the process was opened successfully; otherwise, false.
 */
bool open_process(std::wstring_view process_name) {
  constexpr auto initial_buffer_length = 1024UZ;
  constexpr auto info_length_mismatch = static_cast<NTSTATUS>(0xc0000004);

  auto system_info_buffer = std::make_unique<char[]>(initial_buffer_length);
  auto system_info_buffer_size = initial_buffer_length;

  while (true) {
    auto length = static_cast<ULONG>(system_info_buffer_size);

    if (
      NtQuerySystemInformation(SystemProcessInformation, system_info_buffer.get(), system_info_buffer_size, &length) !=
      info_length_mismatch) {
      break;
    }

    system_info_buffer_size = length;
    system_info_buffer = std::make_unique<char[]>(static_cast<size_t>(length));
  }

  const auto* process = reinterpret_cast<const SYSTEM_PROCESS_INFORMATION*>(system_info_buffer.get());

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wvoid-pointer-to-int-cast"

  while (process->NextEntryOffset != 0) {
    if (process->ImageName.Buffer != nullptr) {
      std::wstring_view name(process->ImageName.Buffer, process->ImageName.Length / 2);

      if (process_name == name) {
        proc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, reinterpret_cast<DWORD>(process->UniqueProcessId));
        return proc != INVALID_HANDLE_VALUE;
      }
    }

    process = reinterpret_cast<const SYSTEM_PROCESS_INFORMATION*>(
      reinterpret_cast<uintptr_t>(process) + process->NextEntryOffset);
  }

#pragma clang diagnostic pop

  return false;
}

/**
 * @brief Scan the memory of the osu! process.
 */
void scan_memory() {
  MEMORY_BASIC_INFORMATION inf;
  for (std::uint8_t* p = nullptr; VirtualQueryEx(proc, p, &inf, sizeof(inf)) != 0; p += inf.RegionSize) {
    if ((inf.State & MEM_COMMIT) == 0 || inf.Protect != PAGE_EXECUTE_READWRITE) {
      continue;
    }

    auto buffer = std::make_unique<std::byte[]>(inf.RegionSize);

    if (ReadProcessMemory(proc, p, buffer.get(), inf.RegionSize, nullptr) == FALSE) {
      continue;
    }

    std::span<const std::byte> data(buffer.get(), inf.RegionSize);

    const auto translate = [&](std::uintptr_t buf_addr) {
      return buf_addr - reinterpret_cast<std::uintptr_t>(buffer.get()) + reinterpret_cast<std::uintptr_t>(p);
    };

    if (time_pointer == 0) {
      if (const auto result = ctfp::find<"DEE983EC04D91C24E8????8B85">(data)) {
        time_pointer = translate(result + 0x1E);

        ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(time_pointer), &time_pointer, sizeof(time_pointer), nullptr);
      }
    }

    if (mode_pointer == 0) {
      if (const auto result = ctfp::find<"A1????3B05????7410">(data)) {
        mode_pointer = translate(result + 0x1);
        ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(mode_pointer), &mode_pointer, sizeof(mode_pointer), nullptr);
      }
    }

    if (gf_ptr_ptr == 0) {
      if (const auto result = ctfp::find<"8B0D????BA010000003909E8????833D">(data)) {
        gf_ptr_ptr = translate(result + 0x2);
        ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(gf_ptr_ptr), &gf_ptr_ptr, sizeof(gf_ptr_ptr), nullptr);
      }
    }

    if (time_pointer != 0 && mode_pointer != 0 && gf_ptr_ptr != 0) {
      break;
    }
  }
}

/**
 * @brief Accept a replay file from the user.
 */
void accept_replay() {
  std::string replay_path;

  std::print("Enter the path to the replay file: ");
  std::getline(std::cin, replay_path);

  active_replay = osp::osr::from_file(replay_path);

  if (active_replay == nullptr) {
    std::println(stderr, "Failed to load the replay file.");
    return;
  }

  const auto& frames = active_replay->frames;
  current_frame = frames.begin();
}

/**
 * @brief Updates input based on the current replay frame.
 */
void update_replay() {
  while (true) {
    if (read_mode() != osu_modes::play) {
      continue;
    }

    const auto audio_time = read_time();
    const auto& frames = active_replay->frames;

    const auto filter = [](const auto& frame, int32_t time) { return frame.time < time; };
    const auto it = std::lower_bound(frames.begin(), frames.end(), audio_time, filter);

    if (it == frames.end()) {
      break;
    }

    const auto check_key = [&](osp::key_state flag, int32_t vk) {
      if ((std::to_underlying(it->keys) & std::to_underlying(flag)) != 0) {
        keybd_event(vk, 0, 0, 0);
      } else {
        keybd_event(vk, 0, KEYEVENTF_KEYUP, 0);
      }
    };

    check_key(osp::key_state::m1, VK_LBUTTON);
    check_key(osp::key_state::m2, VK_RBUTTON);
    check_key(osp::key_state::k1, 'Z');
    check_key(osp::key_state::k2, 'X');
    check_key(osp::key_state::smoke, 'C');

    /**
     * Interpolation is left as an exercise for the reader. :)
     */

    const auto scaled_pos = scale(it->position);

    SetCursorPos(static_cast<int>(scaled_pos.x), static_cast<int>(scaled_pos.y));

    std::this_thread::sleep_for(update_rate);
  }
}

}  // namespace

int32_t main() {
  std::println("Replay Player Example Program");
  std::println("Raw-input must be disabled for this program to work.");

  if (!open_process(L"osu!.exe")) {
    std::println(stderr, "Failed to find the osu! process.");
    return EXIT_FAILURE;
  }

  scan_memory();
  accept_replay();
  update_replay();

  return EXIT_SUCCESS;
}
