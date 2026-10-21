# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>
project('ufuse', 'c',
  version: '0.1.0',
  license: 'BSD-3-Clause',
  meson_version: '>=0.59',
  default_options: [
    'c_std=gnu11',
    'warning_level=3',
    'werror=true',
    'buildtype=release',
  ],
)

inc = include_directories('include')
install_headers('include/ufuse.h')

ufuse_dep = declare_dependency(include_directories: inc)
meson.override_dependency('ufuse', ufuse_dep)

pkg = import('pkgconfig')
pkg.generate(
  name: 'ufuse',
  filebase: 'ufuse',
  description: 'The FUSE and CUSE wire protocol, header-only',
)

subdir('example')
subdir('tests')
