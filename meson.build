project(
  'storage_wiper',
  'cpp',
  version: '1.3.2',
  license: 'MIT',
  default_options: [
    'cpp_std=c++23',
    'warning_level=3',      # Enables -Wall -Wextra
    'werror=true',          # Treat warnings as errors
    'buildtype=release',
    'b_ndebug=if-release',
    'optimization=3',       # -O3 for release builds
  ],
  meson_version: '>=0.59.0'
)

# Project information
project_description = 'Secure disk wiping tool with GTK4/libadwaita interface'

# Compiler
cpp = meson.get_compiler('cpp')

# Additional compiler flags (not covered by warning_level)
add_project_arguments(
  cpp.get_supported_arguments([
    '-Wpedantic',
  ]),
  language: 'cpp'
)

# Dependencies
gtk4_dep = dependency('gtk4', version: '>=4.0', required: true)
gtkmm_dep = dependency('gtkmm-4.0', version: '>=4.6', required: true)
adwaita_dep = dependency('libadwaita-1', version: '>=1.0', required: true)
polkit_dep = dependency('polkit-gobject-1', required: true)
gio_dep = dependency('gio-2.0', required: true)

# Generate config header with version info
config_data = configuration_data()
config_data.set_quoted('PROJECT_VERSION', meson.project_version())
config_data.set_quoted('PROJECT_NAME', meson.project_name())
configure_file(
  output: 'config.h',
  configuration: config_data
)

# GNOME module for GResource compilation
gnome = import('gnome')

# Compile GResource bundle
resources = gnome.compile_resources(
  'storage-wiper-resources',
  'data/resources/storage-wiper.gresource.xml',
  source_dir: 'data/resources',
  c_name: 'storage_wiper'
)

# Source files for main GUI application
sources = files(
  'src/main.cpp',
  'src/Application.cpp',
  'src/viewmodels/MainViewModel.cpp',
  'src/views/MainWindow.cpp',
  'src/views/MainWindowContent.cpp',
  'src/views/DiskRow.cpp',
  'src/views/AlgorithmRow.cpp',
  'src/services/DBusClient.cpp',
)

# Source files shared between GUI and helper (algorithms)
shared_sources = files(
  'src/algorithms/ZeroFillAlgorithm.cpp',
  'src/algorithms/RandomFillAlgorithm.cpp',
  'src/algorithms/DoD522022MAlgorithm.cpp',
  'src/algorithms/SchneierAlgorithm.cpp',
  'src/algorithms/VSITRAlgorithm.cpp',
  'src/algorithms/GutmannAlgorithm.cpp',
  'src/algorithms/GOSTAlgorithm.cpp',
  'src/algorithms/ATASecureEraseAlgorithm.cpp',
  'src/algorithms/VerificationHelper.cpp',
)

# Utility sources (shared)
util_sources = files(
  'src/util/Logger.cpp',
)

# Source files for privileged helper
helper_sources = files(
  'src/helper/main.cpp',
  'src/helper/services/DiskService.cpp',
  'src/helper/services/WipeService.cpp',
  'src/helper/services/SmartService.cpp',
)

# CLI sources
cli_sources = files(
  'src/cli/main.cpp',
  'src/cli/CliApplication.cpp',
  'src/cli/ProgressDisplay.cpp',
  'src/services/DBusClient.cpp',
)

# Header files (for IDE support)
headers = files(
  'src/Application.hpp',
  # Core infrastructure
  'src/core/Command.hpp',
  'src/core/Observable.hpp',
  # Dependency injection
  'src/di/Container.hpp',
  # Models
  'src/models/DiskInfo.hpp',
  'src/models/ViewTypes.hpp',
  'src/models/WipeTypes.hpp',
  # Services
  'src/services/IDiskService.hpp',
  'src/services/IWipeService.hpp',
  'src/helper/services/DiskService.hpp',
  'src/helper/services/WipeService.hpp',
  'src/services/DBusClient.hpp',
  # ViewModels
  'src/viewmodels/MainViewModel.hpp',
  # Views
  'src/views/AlgorithmRow.hpp',
  'src/views/DiskRow.hpp',
  'src/views/MainWindow.hpp',
  'src/views/MainWindowContent.hpp',
  # Algorithms
  'src/algorithms/IWipeAlgorithm.hpp',
  'src/algorithms/ZeroFillAlgorithm.hpp',
  'src/algorithms/RandomFillAlgorithm.hpp',
  'src/algorithms/DoD522022MAlgorithm.hpp',
  'src/algorithms/SchneierAlgorithm.hpp',
  'src/algorithms/VSITRAlgorithm.hpp',
  'src/algorithms/GutmannAlgorithm.hpp',
  'src/algorithms/GOSTAlgorithm.hpp',
  'src/algorithms/ATASecureEraseAlgorithm.hpp',
  # Utilities
  'src/util/FileDescriptor.hpp',
  'src/util/Result.hpp',
  'src/util/Logger.hpp',
  # Helper services
  'src/helper/services/SmartService.hpp',
  # Algorithms
  'src/algorithms/VerificationHelper.hpp',
  # CLI
  'src/cli/CliApplication.hpp',
  'src/cli/ProgressDisplay.hpp',
)

# Include directories
inc = include_directories('src')

# Main GUI application
executable(
  'storage_wiper',
  sources,
  shared_sources,
  util_sources,
  resources,
  include_directories: inc,
  dependencies: [gtk4_dep, gtkmm_dep, adwaita_dep, gio_dep],
  install: true,
  cpp_args: []
)

# Privileged helper daemon
executable(
  'storage-wiper-helper',
  helper_sources,
  shared_sources,
  util_sources,
  include_directories: inc,
  dependencies: [gio_dep, polkit_dep],
  install: true,
  install_dir: get_option('libdir') / 'storage-wiper',
  cpp_args: []
)

# CLI application
executable(
  'storage-wiper-cli',
  cli_sources,
  util_sources,
  include_directories: inc,
  dependencies: [gio_dep],
  install: true,
  cpp_args: []
)

# Desktop file installation
install_data(
  'data/su.kidoz.storage_wiper.desktop',
  install_dir: get_option('datadir') / 'applications'
)

# Icon installation
install_data(
  'data/resources/icons/hicolor/scalable/apps/storage-wiper.svg',
  install_dir: get_option('datadir') / 'icons/hicolor/scalable/apps'
)

# PolicyKit policy installation (for D-Bus helper authorization)
install_data(
  'data/su.kidoz.storage_wiper.policy',
  install_dir: get_option('datadir') / 'polkit-1/actions'
)

# D-Bus system service file (for on-demand helper activation)
# Use configure_file to substitute the correct libdir path
configure_file(
  input: 'data/dbus/su.kidoz.storage_wiper.Helper.service.in',
  output: 'su.kidoz.storage_wiper.Helper.service',
  configuration: {
    'LIBDIR': get_option('prefix') / get_option('libdir')
  },
  install: true,
  install_dir: get_option('datadir') / 'dbus-1/system-services'
)

# D-Bus system bus configuration (allows service to use system bus)
install_data(
  'data/dbus/su.kidoz.storage_wiper.Helper.conf',
  install_dir: get_option('datadir') / 'dbus-1/system.d'
)

# Systemd service file for D-Bus helper
configure_file(
  input: 'data/dbus/storage-wiper-helper.service.in',
  output: 'storage-wiper-helper.service',
  configuration: {
    'LIBDIR': get_option('prefix') / get_option('libdir')
  },
  install: true,
  install_dir: '/usr/lib/systemd/system'
)

# AppStream metainfo for software centers (GNOME Software, KDE Discover)
install_data(
  'data/su.kidoz.storage_wiper.metainfo.xml',
  install_dir: get_option('datadir') / 'metainfo'
)

# All source files for static analysis
all_sources = sources + shared_sources + util_sources + helper_sources + cli_sources

# Optional: clang-tidy support
clang_tidy = find_program('clang-tidy', required: false)
if clang_tidy.found() and get_option('enable_clang_tidy')
  message('clang-tidy found: @0@'.format(clang_tidy.full_path()))
  run_target(
    'clang-tidy',
    command: [
      clang_tidy,
      '-p', meson.project_build_root(),
      '--header-filter=.*',
      all_sources
    ]
  )
endif

# Optional: cppcheck support
cppcheck = find_program('cppcheck', required: false)
if cppcheck.found() and get_option('enable_cppcheck')
  message('cppcheck found: @0@'.format(cppcheck.full_path()))
  run_target(
    'cppcheck',
    command: [
      cppcheck,
      '--enable=warning,style,performance,portability',
      '--inline-suppr',
      '--suppressions-list=@0@/.cppcheck-suppressions'.format(meson.project_source_root()),
      '--std=c++23',
      '--project=@0@/compile_commands.json'.format(meson.project_build_root()),
    ]
  )
endif

# Optional: Clang Static Analyzer (scan-build) support
scan_build = find_program('scan-build', required: false)
if scan_build.found()
  message('scan-build found: @0@'.format(scan_build.full_path()))
endif

# ========== Testing Configuration ==========
if get_option('enable_tests')
  # Google Test dependencies
  gtest_dep = dependency('gtest', required: true)
  gmock_dep = dependency('gmock', required: true)

  # Test sources
  test_sources = files(
    'tests/test_main.cpp',
    'tests/unit/di/ContainerTest.cpp',
    'tests/unit/algorithms/ZeroFillAlgorithmTest.cpp',
    'tests/unit/algorithms/RandomFillAlgorithmTest.cpp',
    'tests/unit/algorithms/DoD522022MAlgorithmTest.cpp',
    'tests/unit/algorithms/SchneierAlgorithmTest.cpp',
    'tests/unit/algorithms/VSITRAlgorithmTest.cpp',
    'tests/unit/algorithms/GOSTAlgorithmTest.cpp',
    'tests/unit/algorithms/GutmannAlgorithmTest.cpp',
    'tests/unit/algorithms/ATASecureEraseAlgorithmTest.cpp',
    'tests/unit/services/WipeServiceTest.cpp',
    'tests/unit/services/DiskServiceTest.cpp',
    'tests/unit/viewmodels/MainViewModelTest.cpp',
  )

  # Test include directories
  test_inc = include_directories('src', 'tests')

  # Additional sources needed for tests (not in shared_sources)
  test_extra_sources = files(
    'src/viewmodels/MainViewModel.cpp',
    'src/helper/services/DiskService.cpp',
    'src/helper/services/WipeService.cpp',
    'src/helper/services/SmartService.cpp',
    'src/util/Logger.cpp',
  )

  # Build test executable
  test_exe = executable(
    'storage_wiper_tests',
    test_sources,
    shared_sources,
    test_extra_sources,
    include_directories: test_inc,
    dependencies: [gtest_dep, gmock_dep, gio_dep, gtkmm_dep],
    cpp_args: ['-DUNIT_TESTING'],
    install: false
  )

  # Register tests with Meson's test runner
  test('unit_tests', test_exe,
    timeout: 300,
    is_parallel: true,
    env: ['GTEST_COLOR=1']
  )

  message('  Tests: enabled')
else
  message('  Tests: disabled (enable with -Denable_tests=true)')
endif

# Configuration summary
message('')
message('Configuration Summary:')
message('  Project: @0@ v@1@'.format(meson.project_name(), meson.project_version()))
message('  Build type: @0@'.format(get_option('buildtype')))
message('  C++ Standard: @0@'.format(get_option('cpp_std')))
message('  GTK4 Version: @0@'.format(gtk4_dep.version()))
message('  gtkmm4 Version: @0@'.format(gtkmm_dep.version()))
message('  Adwaita Version: @0@'.format(adwaita_dep.version()))
message('  Install prefix: @0@'.format(get_option('prefix')))
message('')
message('Static Analysis Tools:')
if clang_tidy.found()
  message('  clang-tidy: found (run: just lint or just lint-parallel)')
else
  message('  clang-tidy: not found (install clang-tools-extra)')
endif
if cppcheck.found()
  message('  cppcheck: found (run: just cppcheck)')
else
  message('  cppcheck: not found (install cppcheck)')
endif
if scan_build.found()
  message('  scan-build: found (run: just scan-build)')
else
  message('  scan-build: not found (install clang-analyzer)')
endif
message('')
message('Memory Analysis:')
valgrind = find_program('valgrind', required: false)
if valgrind.found()
  message('  valgrind: found (run: just valgrind)')
else
  message('  valgrind: not found (install valgrind)')
endif
message('  sanitizers: use just build-asan, build-ubsan, build-tsan')
message('')
