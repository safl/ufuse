# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

# The same unit test compiled as C and as C++, which is also what shows the header works from both
threads = dependency('threads')
test('unit-c', executable('test_unit_c', 'test_unit.c', dependencies: [ufuse_dep, threads]))
if add_languages('cpp', required: false, native: false)
  test('unit-cxx', executable('test_unit_cxx', 'test_unit_cxx.cpp', dependencies: [ufuse_dep, threads],
                              override_options: ['cpp_std=c++17']))
endif

# Mount a filesystem and create a character device; both need root, and skip without it
test('hellofs', find_program('test_hellofs.sh'), args: [hellofs.full_path()], depends: hellofs,
     is_parallel: false, timeout: 120)
test('cuseecho', find_program('test_cuseecho.sh'), args: [cuseecho.full_path()], depends: cuseecho,
     is_parallel: false, timeout: 120)
