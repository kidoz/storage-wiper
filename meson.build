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
adwaita_dep = dependency('libadwaita-1', version: '>=1.0', required: true)

# Source files
sources = files(
  'src/main.cpp',
  'src/Application.cpp',
  'src/views/MainWindow.cpp',
  'src/viewmodels/MainViewModel.cpp',
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

# Header files (for IDE support)
headers = files(
  'src/Application.hpp',
  'src/di/Container.hpp',
  'src/mvvm/Observable.hpp',
  'src/mvvm/Command.hpp',
  'src/views/MainWindow.hpp',
  'src/viewmodels/MainViewModel.hpp',
  'src/services/DiskService.hpp',
  'src/services/WipeService.hpp',
  'src/interfaces/IDiskService.hpp',
  'src/interfaces/IWipeService.hpp',
  'src/algorithms/IWipeAlgorithm.hpp',
  'src/algorithms/ZeroFillAlgorithm.hpp',
  'src/algorithms/RandomFillAlgorithm.hpp',
  'src/algorithms/DoD522022MAlgorithm.hpp',
  'src/algorithms/SchneierAlgorithm.hpp',
  'src/algorithms/VSITRAlgorithm.hpp',
  'src/algorithms/GutmannAlgorithm.hpp',
  'src/algorithms/GOSTAlgorithm.hpp',
  'src/algorithms/ATASecureEraseAlgorithm.hpp',
  'src/util/FileDescriptor.hpp',
  'src/util/Result.hpp',
)

# Include directories
inc = include_directories('src')

# Executable
executable(
  'storage_wiper',
  sources,
  include_directories: inc,
  dependencies: [gtk4_dep, adwaita_dep],
  install: true,
  cpp_args: []
)

# Desktop file installation
install_data(
  'data/com.github.storage-wiper.desktop',
  install_dir: get_option('datadir') / 'applications'
)

# Icon installation
install_data(
  'data/icons/hicolor/scalable/apps/storage-wiper.svg',
  install_dir: get_option('datadir') / 'icons/hicolor/scalable/apps'
)

# PolicyKit policy installation (for pkexec privilege elevation)
install_data(
  'data/com.github.storage-wiper.policy',
  install_dir: get_option('datadir') / 'polkit-1/actions'
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
