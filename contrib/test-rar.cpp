#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

struct Entry {
  std::string name;
  fs::path path;
  uintmax_t size;
};

static std::wstring widen(const std::string &s) {
#ifdef _WIN32
  if (s.empty())
    return std::wstring();
  int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  if (len <= 0)
    return std::wstring(s.begin(), s.end());
  std::wstring out(static_cast<size_t>(len - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), len);
  return out;
#else
  return std::wstring(s.begin(), s.end());
#endif
}

static std::string narrow(const fs::path &p) {
  return p.generic_u8string();
}

static std::string normalize_relative(const fs::path &p) {
  std::string s = p.generic_u8string();
  while (!s.empty() && s.front() == '/')
    s.erase(s.begin());
  return s;
}

#ifdef _WIN32
static int run_command(const std::wstring &cmd, bool verbose) {
  if (verbose)
    std::wcerr << L"unrar command: " << cmd << L"\n";

  std::wstring mutable_cmd = cmd;
  STARTUPINFOW si;
  PROCESS_INFORMATION pi;
  ZeroMemory(&si, sizeof(si));
  ZeroMemory(&pi, sizeof(pi));
  si.cb = sizeof(si);

  if (!CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE, 0,
          nullptr, nullptr, &si, &pi)) {
    std::wcerr << L"CreateProcessW failed: " << GetLastError() << L"\n";
    return 1;
  }

  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD exit_code = 1;
  GetExitCodeProcess(pi.hProcess, &exit_code);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return static_cast<int>(exit_code);
}

static std::wstring quote_arg(const fs::path &p) {
  std::wstring s = p.wstring();
  std::wstring out = L"\"";
  for (wchar_t c : s) {
    if (c == L'"')
      out += L'\\';
    out += c;
  }
  out += L"\"";
  return out;
}

static std::wstring quote_arg(const std::wstring &s) {
  std::wstring out = L"\"";
  for (wchar_t c : s) {
    if (c == L'"')
      out += L'\\';
    out += c;
  }
  out += L"\"";
  return out;
}
#else
static std::string quote_arg(const fs::path &p) {
  std::string s = p.string();
  std::string out = "'";
  for (char c : s) {
    if (c == '\'')
      out += "'\\''";
    else
      out += c;
  }
  out += "'";
  return out;
}

static std::string quote_arg(const std::string &s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'')
      out += "'\\''";
    else
      out += c;
  }
  out += "'";
  return out;
}
#endif

static fs::path make_temp_dir() {
  auto now = std::chrono::steady_clock::now().time_since_epoch().count();
#ifdef _WIN32
  int pid = _getpid();
#else
  int pid = getpid();
#endif
  fs::path root = fs::temp_directory_path() /
      ("test-rar-" + std::to_string(pid) + "-" + std::to_string(now));
  fs::create_directories(root);
  return root;
}

static std::vector<Entry> list_files(const fs::path &root) {
  std::vector<Entry> entries;
  for (const auto &de : fs::recursive_directory_iterator(root)) {
    if (!de.is_regular_file())
      continue;
    fs::path rel = fs::relative(de.path(), root);
    entries.push_back({normalize_relative(rel), de.path(), de.file_size()});
  }
  std::sort(entries.begin(), entries.end(),
      [](const Entry &a, const Entry &b) { return a.name < b.name; });
  return entries;
}

static int run_unrar(const fs::path &unrar, const fs::path &archive,
    const fs::path &dest, bool verbose) {
  fs::create_directories(dest);
#ifdef _WIN32
  std::wstring cmd = quote_arg(unrar) + L" x -idq -o+ -y " +
      quote_arg(archive) + L" " + quote_arg(dest);
  return run_command(cmd, verbose);
#else
  std::string cmd = quote_arg(unrar) + " x -idq -o+ -y " +
      quote_arg(archive) + " " + quote_arg(dest);
  if (verbose)
    std::cerr << "unrar command: " << cmd << "\n";
  return std::system(cmd.c_str());
#endif
}

static int extract_libarchive(const fs::path &archive_path,
    const fs::path &dest) {
  fs::create_directories(dest);

  struct archive *a = archive_read_new();
  archive_read_support_filter_all(a);
  archive_read_support_format_rar(a);
  archive_read_support_format_rar5(a);

  int r;
#ifdef _WIN32
  r = archive_read_open_filename_w(a, archive_path.c_str(), 10240);
#else
  r = archive_read_open_filename(a, archive_path.string().c_str(), 10240);
#endif
  if (r != ARCHIVE_OK) {
    std::cerr << "libarchive open failed: " << archive_error_string(a) << "\n";
    archive_read_free(a);
    return 1;
  }

  fs::path old_cwd = fs::current_path();
  fs::current_path(dest);

  struct archive_entry *entry = nullptr;
  int exit_status = 0;
  const int flags = ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM |
      ARCHIVE_EXTRACT_SECURE_NODOTDOT | ARCHIVE_EXTRACT_SECURE_NOABSOLUTEPATHS;
  while ((r = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
    int er = archive_read_extract(a, entry, flags);
    if (er != ARCHIVE_OK && er != ARCHIVE_WARN) {
      std::cerr << "libarchive extract failed for "
                << archive_entry_pathname(entry) << ": "
                << archive_error_string(a) << "\n";
      exit_status = 1;
      break;
    }
  }

  if (r != ARCHIVE_EOF && exit_status == 0) {
    std::cerr << "libarchive read failed: " << archive_error_string(a) << "\n";
    exit_status = 1;
  }

  fs::current_path(old_cwd);
  archive_read_close(a);
  archive_read_free(a);
  return exit_status;
}

static bool compare_file_bytes(const Entry &a, const Entry &b) {
  if (a.size != b.size) {
    std::cerr << "size mismatch for " << a.name << ": unrar=" << a.size
              << " libarchive=" << b.size << "\n";
    return false;
  }

  std::ifstream af(a.path, std::ios::binary);
  std::ifstream bf(b.path, std::ios::binary);
  if (!af || !bf) {
    std::cerr << "failed to open extracted file " << a.name << "\n";
    return false;
  }

  std::vector<char> abuf(1024 * 1024);
  std::vector<char> bbuf(1024 * 1024);
  uintmax_t offset = 0;
  while (af || bf) {
    af.read(abuf.data(), static_cast<std::streamsize>(abuf.size()));
    bf.read(bbuf.data(), static_cast<std::streamsize>(bbuf.size()));
    std::streamsize an = af.gcount();
    std::streamsize bn = bf.gcount();
    if (an != bn || std::memcmp(abuf.data(), bbuf.data(),
            static_cast<size_t>(an)) != 0) {
      std::streamsize n = std::min(an, bn);
      std::streamsize i = 0;
      while (i < n && abuf[static_cast<size_t>(i)] ==
             bbuf[static_cast<size_t>(i)])
        ++i;
      std::cerr << "content mismatch for " << a.name << " at byte "
                << (offset + static_cast<uintmax_t>(i)) << "\n";
      return false;
    }
    offset += static_cast<uintmax_t>(an);
  }
  return true;
}

static bool compare_trees(const fs::path &unrar_dir,
    const fs::path &libarchive_dir) {
  auto unrar_files = list_files(unrar_dir);
  auto libarchive_files = list_files(libarchive_dir);

  if (unrar_files.size() != libarchive_files.size()) {
    std::cerr << "file count mismatch: unrar=" << unrar_files.size()
              << " libarchive=" << libarchive_files.size() << "\n";
    return false;
  }

  for (size_t i = 0; i < unrar_files.size(); ++i) {
    if (unrar_files[i].name != libarchive_files[i].name) {
      std::cerr << "file name mismatch at index " << i << ": unrar="
                << unrar_files[i].name << " libarchive="
                << libarchive_files[i].name << "\n";
      return false;
    }
  }

  for (size_t i = 0; i < unrar_files.size(); ++i) {
    if (!compare_file_bytes(unrar_files[i], libarchive_files[i]))
      return false;
  }

  std::cout << "OK: " << unrar_files.size() << " files match\n";
  return true;
}

static void usage() {
  std::cerr << "usage: test-rar [--unrar PATH] [--keep-temp] [--verbose] ARCHIVE\n";
}

static int test_rar_main(int argc,
#ifdef _WIN32
    wchar_t **argv
#else
    char **argv
#endif
) {
#ifdef _WIN32
  fs::path unrar = _wgetenv(L"TEST_RAR_UNRAR") ?
      fs::path(_wgetenv(L"TEST_RAR_UNRAR")) : fs::path(L"unrar");
#else
  fs::path unrar = std::getenv("TEST_RAR_UNRAR") ?
      fs::path(std::getenv("TEST_RAR_UNRAR")) : fs::path("unrar");
#endif
  bool keep_temp = false;
  bool verbose = false;
  fs::path archive_path;

  for (int i = 1; i < argc; ++i) {
#ifdef _WIN32
    std::wstring arg = argv[i];
    if (arg == L"--unrar" && i + 1 < argc) {
      unrar = argv[++i];
    } else if (arg == L"--keep-temp") {
      keep_temp = true;
    } else if (arg == L"--verbose") {
      verbose = true;
    } else if (arg == L"--help" || arg == L"-h") {
      usage();
      return 0;
    } else if (archive_path.empty()) {
      archive_path = argv[i];
    } else {
      usage();
      return 2;
    }
#else
    std::string arg = argv[i];
    if (arg == "--unrar" && i + 1 < argc) {
      unrar = widen(argv[++i]);
    } else if (arg == "--keep-temp") {
      keep_temp = true;
    } else if (arg == "--verbose") {
      verbose = true;
    } else if (arg == "--help" || arg == "-h") {
      usage();
      return 0;
    } else if (archive_path.empty()) {
      archive_path = widen(arg);
    } else {
      usage();
      return 2;
    }
#endif
  }

  if (archive_path.empty()) {
    usage();
    return 2;
  }

  fs::path temp = make_temp_dir();
  fs::path unrar_dir = temp / "unrar";
  fs::path libarchive_dir = temp / "libarchive";
  int status = 1;

  try {
    int ur = run_unrar(unrar, archive_path, unrar_dir, verbose);
    if (ur != 0) {
      std::cerr << "unrar failed with exit status " << ur << "\n";
      status = 1;
    } else if (extract_libarchive(archive_path, libarchive_dir) != 0) {
      status = 1;
    } else {
      status = compare_trees(unrar_dir, libarchive_dir) ? 0 : 1;
    }
  } catch (const std::exception &e) {
    std::cerr << "test-rar failed: " << e.what() << "\n";
    status = 1;
  }

  if (keep_temp) {
    std::cerr << "kept temp directory: " << narrow(temp) << "\n";
  } else {
    std::error_code ec;
    fs::remove_all(temp, ec);
  }

  return status;
}

#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
  return test_rar_main(argc, argv);
}
#else
int main(int argc, char **argv) {
  return test_rar_main(argc, argv);
}
#endif
