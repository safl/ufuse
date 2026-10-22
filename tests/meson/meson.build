# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

# A consumer of ufuse, built by check.sh against an installed ufuse and through a wrap of the checkout
project('ufuse-consumer', 'c', default_options: ['c_std=gnu11', 'warning_level=3', 'werror=true'])

executable('hellofs', 'hellofs.c', dependencies: dependency('ufuse'))
