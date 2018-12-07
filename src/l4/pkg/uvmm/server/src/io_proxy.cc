/*
 * Copyright (C) 2016 Kernkonzept GmbH.
 * Author(s): Sarah Hoffmann <sarah.hoffmann@kernkonzept.com>
 *
 * This file is distributed under the terms of the GNU General Public
 * License, version 2.  Please see the COPYING-GPL-2 file for details.
 */

#include "device_factory.h"
#include "device_tree.h"
#include "guest.h"
#include "irq_dt.h"
#include "io_proxy.h"
#include "virt_bus.h"

static Dbg trace(Dbg::Dev, Dbg::Trace, "ioproxy");
static Dbg info(Dbg::Dev, Dbg::Info, "ioproxy");
static Dbg warn(Dbg::Dev, Dbg::Warn, "ioproxy");

namespace {

  void
  bind_irq(Vmm::Guest *vmm, Vmm::Virt_bus *vbus, Gic::Ic *ic,
           unsigned dt_irq, unsigned io_irq, char const *dev_name)
  {
    info.printf("IO device '%s' - registering irq 0x%x -> 0x%x\n",
                dev_name, io_irq, dt_irq);
    if (!ic->get_irq_source(dt_irq))
      {
        auto irq_svr = Vdev::make_device<Vdev::Irq_svr>(io_irq);

        L4Re::chkcap(vmm->registry()->register_irq_obj(irq_svr.get()),
                     "Invalid capability");

        // We have a 1:1 association, so if the irq is not bound yet we
        // should be able to bind the icu irq
        L4Re::chksys(vbus->icu()->bind(io_irq, irq_svr->obj_cap()),
                     "Cannot bind to IRQ");

        // Point irq_svr to ic:dt_irq for upstream events (like
        // interrupt delivery)
        irq_svr->set_sink(ic, dt_irq);

        // Point ic to irq_svr for downstream events (like eoi handling)
        ic->bind_irq_source(dt_irq, irq_svr);

        irq_svr->eoi();
        return;
      }

    warn.printf("IO device '%s': irq 0x%x -> 0x%x already registered\n",
                dev_name, io_irq, dt_irq);

    // Ensure we have the correct binding of the currently registered
    // source
    auto irq_source = ic->get_irq_source(dt_irq);
    auto other_svr = dynamic_cast<Vdev::Irq_svr const *>(irq_source.get());
    if (other_svr && (io_irq == other_svr->get_io_irq()))
      return;

    if (other_svr)
      Err().printf("bind_irq: ic:0x%x -> 0x%x -- "
                   "irq already bound to different io irq: 0x%x  \n",
                   dt_irq, io_irq, other_svr->get_io_irq());
    else
      Err().printf("ic:0x%x is bound to a different irq type\n",
                   dt_irq);
    throw L4::Runtime_error(-L4_EEXIST);
  }

  unsigned num_reg_entries(Vdev::Dt_node const &node)
  {
    if (!node.has_mmio_regs())
      return 0;

    for (unsigned num = 0;; ++num)
      {
        l4_uint64_t dtaddr, dtsize;
        int ret = node.get_reg_val(num, &dtaddr, &dtsize);

        if (ret == -Vdev::Dt_node::ERR_BAD_INDEX)
          return num;

        if (ret < 0)
          L4Re::chksys(-L4_EINVAL, "Check reg descriptor in device tree.");
      }

    return 0;
  }

  unsigned num_interrupts(Vdev::Device_lookup *devs, Vdev::Dt_node const &node)
  {
    if (!node.has_irqs())
      return 0;

    auto it = Vdev::Irq_dt_iterator(devs, node);

    for (unsigned num = 0;; ++num)
      {
        int ret = it.next(devs);

        if (ret == -L4_ERANGE)
          return num;

        if (ret < 0)
          L4Re::chksys(ret, "Check interrupt descriptions in device tree");
      }

    return 0;
  }
}


namespace Vdev {

// default set to false
static bool phys_dev_prepared;

void
Io_proxy::prepare_factory(Device_lookup const *devs)
{
  devs->vbus()->collect_resources(devs);
  phys_dev_prepared = true;
}

namespace {

struct F : Factory
{
  static bool check_regs(Device_lookup const *devs,
                         Dt_node const &node)
  {
    if (!node.has_prop("reg"))
      return true;

    auto vmm = devs->vmm();
    l4_uint64_t addr, size;
    for (int index = 0; /* no condition */ ; ++index)
      {
        int res = node.get_reg_val(index, &addr, &size);
        switch (res)
          {
          case 0:
            if (!vmm->mmio_region_valid(Vmm::Guest_addr(addr), size))
              return false;
            break;
          case -Dt_node::ERR_BAD_INDEX:
            // reached end of reg entries
            return true;
          case -Dt_node::ERR_NOT_TRANSLATABLE:
            // region not managed by us
            continue;
          case -Dt_node::ERR_RANGE:
            info.printf("Reg entry too large '%s'.reg[%d]\n",
                        node.get_name(), index);
            return false;
          default:
            Err().printf("Invalid reg entry '%s'.reg[%d]: %s\n",
                         node.get_name(), index, fdt_strerror(res));
            return false;
          }
      }
  }

  bool check_and_bind_irqs(Device_lookup *devs, Dt_node const &node)
  {
    if (!node.has_irqs())
      return true;

    // Check whether all IRQs are available
    auto vbus = devs->vbus().get();

    Irq_dt_iterator it(devs, node);
    do
      {
        if (it.next(devs) < 0)
          return false;

        // Check that the IRQ is available on the vbus when a
        // virtual interrupt handler needs to be connected.
        if (it.ic_is_virt() && ! vbus->irq_present(it.irq()))
          return false;
      }
    while (it.has_next());

    // Now bind the IRQs.
    it = Irq_dt_iterator(devs, node);
    do
      {
        it.next(devs);

        if (it.ic_is_virt())
          {
            int dt_irq = it.irq();
            bind_irq(devs->vmm(), vbus, it.ic().get(), dt_irq, dt_irq,
                     node.get_name());
            vbus->mark_irq_bound(dt_irq);
          }
      }
    while (it.has_next());

    return true;
  }

  cxx::Ref_ptr<Device> create_from_vbus_dev(Device_lookup *devs,
                                            Dt_node const &node,
                                            char const *hid)
  {
    auto *vdev = devs->vbus()->find_unassigned_device_by_hid(hid);

    if (!vdev)
      {
        warn.printf("%s: requested vbus device '%s' not available.\n",
                    node.get_name(), hid);
        return nullptr;
      }

    // Count number of expected resources as a cheap means of validation.
    // This also checks that the device tree properties are correctly parsable.
    unsigned todo_regs = num_reg_entries(node);
    unsigned todo_irqs = num_interrupts(devs, node);

    // collect resources directly for device
    auto vbus = devs->vbus().get();
    for (unsigned i = 0; i < vdev->dev_info().num_resources; ++i)
      {
        l4vbus_resource_t res;

        L4Re::chksys(vdev->io_dev().get_resource(i, &res),
                     "Cannot get resource in collect_resources");

        char const *resname = reinterpret_cast<char const *>(&res.id);

        if (res.type == L4VBUS_RESOURCE_MEM)
          {
            if (strncmp(resname, "reg", 3) || resname[3] < '0' || resname[3] > '9')
              {
                warn.printf("%s: Vbus memory resource '%.4s' has no recognisable name.\n",
                            node.get_name(), resname);
                continue;
              }

            unsigned resid = resname[3] - '0';
            l4_uint64_t dtaddr, dtsize;
            L4Re::chksys(node.get_reg_val(resid, &dtaddr, &dtsize),
                         "Match reg entry of device entry with vbus resource.");

            if (res.end - res.start + 1 != dtsize)
              L4Re::chksys(-L4_ENOMEM,
                           "Matching resource size of VBUS resource and device tree entry");

            trace.printf("Adding MMIO resource %s.%.4s : [0x%lx - 0x%lx] -> [0x%llx - 0x%llx]\n",
                      vdev->dev_info().name, resname, res.start, res.end,
                      dtaddr, dtaddr + (dtsize - 1));

            auto handler = Vdev::make_device<Ds_handler>(vbus->io_ds(),
                                                         0, dtsize, res.start);

            devs->vmm()->add_mmio_device(Vmm::Region::ss(Vmm::Guest_addr(dtaddr), dtsize),
                                         handler);
            --todo_regs;
          }
        else if (res.type == L4VBUS_RESOURCE_IRQ)
          {
            if (strncmp(resname, "irq", 3) || resname[3] < '0' || resname[3] > '9')
              {
                warn.printf("%s: Vbus memory resource '%.4s' has no recognisable name.\n",
                            node.get_name(), resname);
                continue;
              }

            unsigned resid = resname[3] - '0';

            if (resid >= todo_irqs)
              {
                Err().printf("%s: VBUS interupts resource '%.4s' has no matching device tree entry.",
                             node.get_name(), resname);
                L4Re::chksys(-L4_ENOMEM,
                             "Matching VBUS interrupt resources against device tree.");
              }

            auto it = Irq_dt_iterator(devs, node);

            for (unsigned n = 0; n < resid; ++n)
              {
                // Just advance the iterator without error checking. num_interrupts()
                // above already checked that 'todo_irqs' interrupts are well defined.
                it.next(devs);
              }

            if (it.ic_is_virt())
              {
                int dt_irq = it.irq();
                bind_irq(devs->vmm(), vbus, it.ic().get(), dt_irq, res.start,
                         node.get_name());
              }

            trace.printf("Registering IRQ resource %s.%.4s : 0x%lx\n",
                         vdev->dev_info().name, resname, res.start);
            --todo_irqs;
          }
      }

    if (todo_regs > 0)
      {
        Err().printf("%s: not enough memory resources found in VBUS device '%s'.\n",
                     node.get_name(), hid);
        L4Re::chksys(-L4_EINVAL, "Match memory resources");
      }
    if (todo_irqs > 0)
      {
        Err().printf("%s: not enough interrupt resources found in VBUS device '%s'.\n",
                     node.get_name(), hid);
        L4Re::chksys(-L4_EINVAL, "Match interrupt resources");
      }

    auto device = make_device<Io_proxy>(vdev->io_dev());

    vdev->set_proxy(device);

    return device;
  }

  cxx::Ref_ptr<Device> create(Device_lookup *devs,
                              Dt_node const &node) override
  {
    char const *prop = node.get_prop<char>("l4vmm,vbus-dev", nullptr);

    if (prop)
      return create_from_vbus_dev(devs, node, prop);

    if (!phys_dev_prepared)
      {
        Err().printf("%s: Io_proxy::create() invoked before prepare_factory()\n"
                     "\tprobably invalid device tree\n", node.get_name());
        return nullptr;
      }

    // Check mmio resources - mmio areas are already established
    if (!check_regs(devs, node))
      return nullptr;

    if (!check_and_bind_irqs(devs, node))
      return nullptr;

    L4vbus::Device io_dev;
    return make_device<Io_proxy>(io_dev);
  }

  F() { pass_thru = this; }
};

static F f;

} // namespace

} // namespace
