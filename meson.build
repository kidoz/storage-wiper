project(
  'storage_wiper',
  'cpp',
  version: '1.0.0',
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
  'src/view_models/MainViewModel.cpp',
  'src/views/MainWindow.cpp',
  'src/views/MainWindowContent.cpp',
  'src/views/DiskRow.cpp',
  'src/views/AlgorithmRow.cpp',
  'src/services/DBusClient.cpp',
)

# Source files shared between GUI and helper (services/algorithms)
shared_sources = files(
  'src/services/DiskService.cpp',
  'src/services/WipeService.cpp',
  'src/algorithms/ZeroFillAlgorithm.cpp',
  'src/algorithms/RandomFillAlgorithm.cpp',
  'src/algorithms/DoD522022MAlgorithm.cpp',
  'src/algorithms/SchneierAlgorithm.cpp',
  'src/algorithms/VSITRAlgorithm.cpp',
  'src/algorithms/GutmannAlgorithm.cpp',
  'src/algorithms/GOSTAlgorithm.cpp',
  'src/algorithms/ATASecureEraseAlgorithm.cpp',
)

# Source files for privileged helper
helper_sources = files(
  'src/helper/main.cpp',
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
  'src/services/DiskService.hpp',
  'src/services/WipeService.hpp',
  'src/services/DBusClient.hpp',
  # ViewModels
  'src/view_models/MainViewModel.hpp',
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
)

# Include directories
inc = include_directories('src')

# Main GUI application
executable(
  'storage_wiper',
  sources,
  shared_sources,
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
  include_directories: inc,
  dependencies: [gio_dep, polkit_dep],
  install: true,
  install_dir: get_option('libdir') / 'storage-wiper',
  cpp_args: []
)

# Desktop file installation
install_data(
  'data/su.kidoz.storage_wiper.desktop',
  install_dir: get_option('datadir') / 'applications'
)

# Icon installation
install_data(
  'data/icons/hicolor/scalable/apps/storage-wiper.svg',
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
      sources
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
if clang_tidy.found()
  message('  clang-tidy: enabled (run: meson compile -C builddir clang-tidy)')
else
  message('  clang-tidy: not found')
endif
if cppcheck.found()
  message('  cppcheck: enabled (run: meson compile -C builddir cppcheck)')
else
  message('  cppcheck: not found')
endif
message('')
