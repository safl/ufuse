# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>
project('ufuse', 'c',
  version: '0.3.0',
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
ufuse_dep = declare_dependency(include_directories: inc)
meson.override_dependency('ufuse', ufuse_dep)

# A parent project that takes ufuse as a subproject only to build against it
# sets skip-install=true, so nothing of ufuse lands in its prefix
if not get_option('skip-install')
  install_headers('include/ufuse.h')
  pkg = import('pkgconfig')
  pkg.generate(
    name: 'ufuse',
    filebase: 'ufuse',
    description: 'The FUSE and CUSE wire protocol, header-only',
    url: 'https://github.com/safl/ufuse',
  )
endif

# A project taking ufuse through a wrap gets the dependency, not the examples and tests
if not meson.is_subproject()
  subdir('example')
  subdir('tests')
endif
