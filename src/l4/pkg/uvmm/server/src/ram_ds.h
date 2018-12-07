/*
 * Copyright (C) 2018 Kernkonzept GmbH.
 * Author(s): Sarah Hoffmann <sarah.hoffmann@kernkonzept.com>
 *
 * This file is distributed under the terms of the GNU General Public
 * License, version 2.  Please see the COPYING-GPL-2 file for details.
 */
#pragma once

#include <l4/re/dataspace>
#include <l4/re/dma_space>
#include <l4/re/util/cap_alloc>
#include <l4/re/util/unique_cap>
#include <l4/util/util.h>

#include <l4/l4virtio/virtqueue>

#include <cstdio>

#include "device.h"
#include "device_tree.h"
#include "mem_types.h"

namespace Vmm {

/**
 * A continuous piece of RAM backed by a part of an L4 dataspace.
 */
class Ram_ds
{
public:
  enum { Ram_base_identity_mapped = ~0UL };

  /**
   * Create a new RAM dataspace.
   *
   * \param ds       L4Re Dataspace that represents the RAM for the VM.
   * \param size     Size of the region (default: use dataspace size).
   * \param offset   Offset into the dataspace.
   */
  Ram_ds(L4::Cap<L4Re::Dataspace> ds, l4_size_t size, l4_addr_t offset)
  : _size(size), _ds_offset(offset), _ds(ds)
  {}

  Ram_ds(Vmm::Ram_ds const &) = delete;
  Ram_ds(Vmm::Ram_ds &&) = default;
  ~Ram_ds() = default;

  /**
   * Set up the memory for DMA and host access.
   *
   * \param vm_base  Guest physical address where the RAM should be mapped.
   *                 If `Ram_base_identity_mapped`, use the host physical address
   *                 of the backing memory (required for DMA without IOMMU).
   */
  long setup(Vmm::Guest_addr vm_base);

  /**
   * Load the contents of the given dataspace into guest RAM.
   *
   * \param file  Dataspace to load from.
   * \param addr  Guest physical address to load the data space to.
   * \param sz    Number of bytes to copy.
   */
  void load_file(L4::Cap<L4Re::Dataspace> const &file,
                 Vmm::Guest_addr addr, l4_size_t sz) const;

  /**
   * Get a VMM-virtual pointer from a guest-physical address
   */
  l4_addr_t guest2host(Vmm::Guest_addr p) const noexcept
  { return p.get() + _offset; }

  L4::Cap<L4Re::Dataspace> ds() const noexcept
  { return _ds; }

  void dt_append_dmaprop(Vdev::Dt_node const &mem_node) const
  {
    int addr_cells = mem_node.get_address_cells();
    mem_node.appendprop("dma-ranges", _phys_ram, addr_cells);
    mem_node.appendprop("dma-ranges", _vm_start.get(), addr_cells);
    mem_node.appendprop("dma-ranges", _phys_size, mem_node.get_size_cells());
  }

  Vmm::Guest_addr vm_start() const noexcept { return _vm_start; }
  l4_size_t size() const noexcept { return _size; }
  l4_addr_t local_start() const noexcept { return _local_start; }
  l4_addr_t ds_offset() const noexcept { return _ds_offset; }

  bool has_phys_addr() const noexcept { return _phys_size > 0; }

private:
  /// Offset between guest-physical and host-virtual address.
  l4_mword_t _offset;
  /// uvmm local address where the dataspace has been mapped.
  l4_addr_t _local_start;
  /// Guest-physical address of the mapped dataspace.
  Vmm::Guest_addr _vm_start;
  /// Size of the mapped area.
  l4_size_t _size;
  /// Offset into the dataspace where the mapped area starts.
  l4_addr_t _ds_offset;

  /// Backing dataspace for the RAM area.
  L4::Cap<L4Re::Dataspace> _ds;
  /// DMA space providing device access (if applicable).
  L4Re::Util::Unique_cap<L4Re::Dma_space> _dma;
  /// Host-physical address of the beginning of the mapped area (if applicable).
  L4Re::Dma_space::Dma_addr _phys_ram;
  /// Size of the continiously mapped area from the beginning of the area.
  l4_size_t _phys_size;
};

} // namespace
