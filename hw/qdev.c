/*
 *  Dynamic device configuration and creation.
 *
 *  Copyright (c) 2009 CodeSourcery
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

/* The theory here is that it should be possible to create a machine without
   knowledge of specific devices.  Historically board init routines have
   passed a bunch of arguments to each device, requiring the board know
   exactly which device it is dealing with.  This file provides an abstract
   API for device configuration and initialization.  Devices will generally
   inherit from a particular bus (e.g. PCI or I2C) rather than
   this API directly.  */

#include "net.h"
#include "qdev.h"
#include "sysemu.h"
#include "monitor.h"
#include "qjson.h"
#include "qbuffer.h"
#include "qint.h"

static int qdev_hotplug = 0;
static bool qdev_hot_added = false;
static bool qdev_hot_removed = false;

/* This is a nasty hack to allow passing a NULL bus to qdev_create.  */
static BusState *main_system_bus;

DeviceInfo *device_info_list;

static BusState *qbus_find_recursive(BusState *bus, const char *name,
                                     const BusInfo *info);

/* Register a new device type.  */
void qdev_register(DeviceInfo *info)
{
    assert(info->size >= sizeof(DeviceState));
    assert(!info->next);

    info->next = device_info_list;
    device_info_list = info;
}

static DeviceInfo *qdev_find_info(BusInfo *bus_info, const char *name)
{
    DeviceInfo *info;

    /* first check device names */
    for (info = device_info_list; info != NULL; info = info->next) {
        if (bus_info && info->bus_info != bus_info)
            continue;
        if (strcmp(info->name, name) != 0)
            continue;
        return info;
    }

    /* failing that check the aliases */
    for (info = device_info_list; info != NULL; info = info->next) {
        if (bus_info && info->bus_info != bus_info)
            continue;
        if (!info->alias)
            continue;
        if (strcmp(info->alias, name) != 0)
            continue;
        return info;
    }
    return NULL;
}

static DeviceState *qdev_create_from_info(BusState *bus, DeviceInfo *info)
{
    DeviceState *dev;

    assert(bus->info == info->bus_info);
    dev = qemu_mallocz(info->size);
    dev->info = info;
    dev->parent_bus = bus;
    qdev_prop_set_defaults(dev, dev->info->props);
    qdev_prop_set_defaults(dev, dev->parent_bus->info->props);
    qdev_prop_set_globals(dev);
    QLIST_INSERT_HEAD(&bus->children, dev, sibling);
    if (qdev_hotplug) {
        assert(bus->allow_hotplug);
        dev->hotplugged = 1;
        qdev_hot_added = true;
    }
    dev->instance_id_alias = -1;
    dev->state = DEV_STATE_CREATED;
    return dev;
}

/* Create a new device.  This only initializes the device state structure
   and allows properties to be set.  qdev_init should be called to
   initialize the actual device emulation.  */
DeviceState *qdev_create(BusState *bus, const char *name)
{
    DeviceState *dev;

    dev = qdev_try_create(bus, name);
    if (!dev) {
        hw_error("Unknown device '%s' for bus '%s'\n", name, bus->info->name);
    }

    return dev;
}

DeviceState *qdev_try_create(BusState *bus, const char *name)
{
    DeviceInfo *info;

    if (!bus) {
        bus = sysbus_get_default();
    }

    info = qdev_find_info(bus->info, name);

    if (!info) {
        return NULL;
    }

    return qdev_create_from_info(bus, info);
}

static void qdev_print_devinfo(DeviceInfo *info)
{
    error_printf("name \"%s\", bus %s",
                 info->name, info->bus_info->name);
    if (info->alias) {
        error_printf(", alias \"%s\"", info->alias);
    }
    if (info->desc) {
        error_printf(", desc \"%s\"", info->desc);
    }
    if (info->no_user) {
        error_printf(", no-user");
    }
    error_printf("\n");
}

static int set_property(const char *name, const char *value, void *opaque)
{
    DeviceState *dev = opaque;

    if (strcmp(name, "driver") == 0)
        return 0;
    if (strcmp(name, "bus") == 0)
        return 0;

    if (qdev_prop_parse(dev, name, value) == -1) {
        return -1;
    }
    return 0;
}

int qdev_device_help(QemuOpts *opts)
{
    const char *driver;
    DeviceInfo *info;
    Property *prop;

    driver = qemu_opt_get(opts, "driver");
    if (driver && !strcmp(driver, "?")) {
        for (info = device_info_list; info != NULL; info = info->next) {
            if (info->no_user) {
                continue;       /* not available, don't show */
            }
            qdev_print_devinfo(info);
        }
        return 1;
    }

    if (!qemu_opt_get(opts, "?")) {
        return 0;
    }

    info = qdev_find_info(NULL, driver);
    if (!info) {
        return 0;
    }

    for (prop = info->props; prop && prop->name; prop++) {
        /*
         * TODO Properties without a parser are just for dirty hacks.
         * qdev_prop_ptr is the only such PropertyInfo.  It's marked
         * for removal.  This conditional should be removed along with
         * it.
         */
        if (!prop->info->parse) {
            continue;           /* no way to set it, don't show */
        }
        error_printf("%s.%s=%s\n", info->name, prop->name, prop->info->name);
    }
    return 1;
}

DeviceState *qdev_device_add(QemuOpts *opts)
{
    const char *driver, *path, *id;
    DeviceInfo *info;
    DeviceState *qdev;
    BusState *bus;

    driver = qemu_opt_get(opts, "driver");
    if (!driver) {
        qerror_report(QERR_MISSING_PARAMETER, "driver");
        return NULL;
    }

    /* find driver */
    info = qdev_find_info(NULL, driver);
    if (!info || info->no_user) {
        qerror_report(QERR_INVALID_PARAMETER_VALUE, "driver", "a driver name");
        error_printf_unless_qmp("Try with argument '?' for a list.\n");
        return NULL;
    }

    /* find bus */
    path = qemu_opt_get(opts, "bus");
    if (path != NULL) {
        bus = qbus_find(path);
        if (!bus) {
            return NULL;
        }
        if (bus->info != info->bus_info) {
            qerror_report(QERR_BAD_BUS_FOR_DEVICE,
                           driver, bus->info->name);
            return NULL;
        }
    } else {
        bus = qbus_find_recursive(main_system_bus, NULL, info->bus_info);
        if (!bus) {
            qerror_report(QERR_NO_BUS_FOR_DEVICE,
                           info->name, info->bus_info->name);
            return NULL;
        }
    }
    if (qdev_hotplug && !bus->allow_hotplug) {
        qerror_report(QERR_BUS_NO_HOTPLUG, bus->name);
        return NULL;
    }

    /* create device, set properties */
    qdev = qdev_create_from_info(bus, info);
    id = qemu_opts_id(opts);
    if (id) {
        qdev->id = id;
    }
    if (qemu_opt_foreach(opts, set_property, qdev, 1) != 0) {
        qdev_free(qdev);
        return NULL;
    }
    if (qdev_init(qdev) < 0) {
        qerror_report(QERR_DEVICE_INIT_FAILED, driver);
        return NULL;
    }
    qdev->opts = opts;
    return qdev;
}

/* Initialize a device.  Device properties should be set before calling
   this function.  IRQs and MMIO regions should be connected/mapped after
   calling this function.
   On failure, destroy the device and return negative value.
   Return 0 on success.  */
int qdev_init(DeviceState *dev)
{
    int rc;

    assert(dev->state == DEV_STATE_CREATED);
    rc = dev->info->init(dev, dev->info);
    if (rc < 0) {
        qdev_free(dev);
        return rc;
    }

    if (dev->info->vmsd) {
        vmstate_register_with_alias_id(dev, -1, dev->info->vmsd, dev,
                                       dev->instance_id_alias,
                                       dev->alias_required_for_version);
    }
    dev->state = DEV_STATE_INITIALIZED;
    return 0;
}

void qdev_set_legacy_instance_id(DeviceState *dev, int alias_id,
                                 int required_for_version)
{
    assert(dev->state == DEV_STATE_CREATED);
    dev->instance_id_alias = alias_id;
    dev->alias_required_for_version = required_for_version;
}

int qdev_unplug(DeviceState *dev)
{
    if (!dev->parent_bus->allow_hotplug) {
        qerror_report(QERR_BUS_NO_HOTPLUG, dev->parent_bus->name);
        return -1;
    }
    assert(dev->info->unplug != NULL);

    qdev_hot_removed = true;

    return dev->info->unplug(dev);
}

static int qdev_reset_one(DeviceState *dev, void *opaque)
{
    if (dev->info->reset) {
        dev->info->reset(dev);
    }

    return 0;
}

BusState *sysbus_get_default(void)
{
    if (!main_system_bus) {
        main_system_bus = qbus_create(&system_bus_info, NULL,
                                      "main-system-bus");
    }
    return main_system_bus;
}

static int qbus_reset_one(BusState *bus, void *opaque)
{
    if (bus->info->reset) {
        return bus->info->reset(bus);
    }
    return 0;
}

void qdev_reset_all(DeviceState *dev)
{
    qdev_walk_children(dev, qdev_reset_one, qbus_reset_one, NULL);
}

void qbus_reset_all_fn(void *opaque)
{
    BusState *bus = opaque;
    qbus_walk_children(bus, qdev_reset_one, qbus_reset_one, NULL);
}

/* can be used as ->unplug() callback for the simple cases */
int qdev_simple_unplug_cb(DeviceState *dev)
{
    /* just zap it */
    qdev_free(dev);
    return 0;
}

/* Like qdev_init(), but terminate program via hw_error() instead of
   returning an error value.  This is okay during machine creation.
   Don't use for hotplug, because there callers need to recover from
   failure.  Exception: if you know the device's init() callback can't
   fail, then qdev_init_nofail() can't fail either, and is therefore
   usable even then.  But relying on the device implementation that
   way is somewhat unclean, and best avoided.  */
void qdev_init_nofail(DeviceState *dev)
{
    DeviceInfo *info = dev->info;

    if (qdev_init(dev) < 0) {
        error_report("Initialization of device %s failed\n", info->name);
        exit(1);
    }
}

/* Unlink device from bus and free the structure.  */
void qdev_free(DeviceState *dev)
{
    BusState *bus;
    Property *prop;

    if (dev->state == DEV_STATE_INITIALIZED) {
        while (dev->num_child_bus) {
            bus = QLIST_FIRST(&dev->child_bus);
            qbus_free(bus);
        }
        if (dev->info->vmsd)
            vmstate_unregister(dev, dev->info->vmsd, dev);
        if (dev->info->exit)
            dev->info->exit(dev);
        if (dev->opts)
            qemu_opts_del(dev->opts);
    }
    QLIST_REMOVE(dev, sibling);
    for (prop = dev->info->props; prop && prop->name; prop++) {
        if (prop->info->free) {
            prop->info->free(dev, prop);
        }
    }
    qemu_free(dev);
}

void qdev_machine_creation_done(void)
{
    /*
     * ok, initial machine setup is done, starting from now we can
     * only create hotpluggable devices
     */
    qdev_hotplug = 1;
}

bool qdev_machine_modified(void)
{
    return qdev_hot_added || qdev_hot_removed;
}

/* Get a character (serial) device interface.  */
CharDriverState *qdev_init_chardev(DeviceState *dev)
{
    static int next_serial;

    /* FIXME: This function needs to go away: use chardev properties!  */
    return serial_hds[next_serial++];
}

BusState *qdev_get_parent_bus(DeviceState *dev)
{
    return dev->parent_bus;
}

void qdev_init_gpio_in(DeviceState *dev, qemu_irq_handler handler, int n)
{
    assert(dev->num_gpio_in == 0);
    dev->num_gpio_in = n;
    dev->gpio_in = qemu_allocate_irqs(handler, dev, n);
}

void qdev_init_gpio_out(DeviceState *dev, qemu_irq *pins, int n)
{
    assert(dev->num_gpio_out == 0);
    dev->num_gpio_out = n;
    dev->gpio_out = pins;
}

qemu_irq qdev_get_gpio_in(DeviceState *dev, int n)
{
    assert(n >= 0 && n < dev->num_gpio_in);
    return dev->gpio_in[n];
}

void qdev_connect_gpio_out(DeviceState * dev, int n, qemu_irq pin)
{
    assert(n >= 0 && n < dev->num_gpio_out);
    dev->gpio_out[n] = pin;
}

void qdev_set_nic_properties(DeviceState *dev, NICInfo *nd)
{
    qdev_prop_set_macaddr(dev, "mac", nd->macaddr);
    if (nd->vlan)
        qdev_prop_set_vlan(dev, "vlan", nd->vlan);
    if (nd->netdev)
        qdev_prop_set_netdev(dev, "netdev", nd->netdev);
    if (nd->nvectors != DEV_NVECTORS_UNSPECIFIED &&
        qdev_prop_exists(dev, "vectors")) {
        qdev_prop_set_uint32(dev, "vectors", nd->nvectors);
    }
}

BusState *qdev_get_child_bus(DeviceState *dev, const char *name)
{
    BusState *bus;

    QLIST_FOREACH(bus, &dev->child_bus, sibling) {
        if (strcmp(name, bus->name) == 0) {
            return bus;
        }
    }
    return NULL;
}

int qbus_walk_children(BusState *bus, qdev_walkerfn *devfn,
                       qbus_walkerfn *busfn, void *opaque)
{
    DeviceState *dev;
    int err;

    if (busfn) {
        err = busfn(bus, opaque);
        if (err) {
            return err;
        }
    }

    QLIST_FOREACH(dev, &bus->children, sibling) {
        err = qdev_walk_children(dev, devfn, busfn, opaque);
        if (err < 0) {
            return err;
        }
    }

    return 0;
}

int qdev_walk_children(DeviceState *dev, qdev_walkerfn *devfn,
                       qbus_walkerfn *busfn, void *opaque)
{
    BusState *bus;
    int err;

    if (devfn) {
        err = devfn(dev, opaque);
        if (err) {
            return err;
        }
    }

    QLIST_FOREACH(bus, &dev->child_bus, sibling) {
        err = qbus_walk_children(bus, devfn, busfn, opaque);
        if (err < 0) {
            return err;
        }
    }

    return 0;
}

static BusState *qbus_find_recursive(BusState *bus, const char *name,
                                     const BusInfo *info)
{
    DeviceState *dev;
    BusState *child, *ret;
    int match = 1;

    if (name && (strcmp(bus->name, name) != 0)) {
        match = 0;
    }
    if (info && (bus->info != info)) {
        match = 0;
    }
    if (match) {
        return bus;
    }

    QLIST_FOREACH(dev, &bus->children, sibling) {
        QLIST_FOREACH(child, &dev->child_bus, sibling) {
            ret = qbus_find_recursive(child, name, info);
            if (ret) {
                return ret;
            }
        }
    }
    return NULL;
}

DeviceState *qdev_find_recursive(BusState *bus, const char *id)
{
    DeviceState *dev, *ret;
    BusState *child;

    QLIST_FOREACH(dev, &bus->children, sibling) {
        if (dev->id && strcmp(dev->id, id) == 0)
            return dev;
        QLIST_FOREACH(child, &dev->child_bus, sibling) {
            ret = qdev_find_recursive(child, id);
            if (ret) {
                return ret;
            }
        }
    }
    return NULL;
}

static void qbus_list_bus(DeviceState *dev)
{
    BusState *child;
    const char *sep = " ";

    error_printf("child busses at \"%s\":",
                 dev->id ? dev->id : dev->info->name);
    QLIST_FOREACH(child, &dev->child_bus, sibling) {
        error_printf("%s\"%s\"", sep, child->name);
        sep = ", ";
    }
    error_printf("\n");
}

static void qbus_list_dev(BusState *bus)
{
    DeviceState *dev;
    const char *sep = " ";

    //if statement added from jan's code
    if (monitor_cur_is_qmp()) {
        return;
    }
    error_printf("devices at \"%s\":", bus->name);
    QLIST_FOREACH(dev, &bus->children, sibling) {
        error_printf("%s\"%s\"", sep, dev->info->name);
        if (dev->id)
            error_printf("/\"%s\"", dev->id);
        sep = ", ";
    }
    error_printf("\n");
}

static BusState *qbus_find_bus(DeviceState *dev, char *elem)
{
    BusState *child;

    QLIST_FOREACH(child, &dev->child_bus, sibling) {
        if (strcmp(child->name, elem) == 0) {
            return child;
        }
    }
    return NULL;
}

static DeviceState *qbus_find_dev(BusState *bus, char *elem)
{
    DeviceState *dev;
    int instance, n;
    char buf[128];

    //XXX: temporary stuff. Remove later

    if (sscanf(elem, "%127[^.].%u", buf, &instance) == 2) {
        elem = buf;
    } else {
        instance = 0;
    }

    n = 0;
    QLIST_FOREACH(dev, &bus->children, sibling) {
        if (strcmp(dev->info->name, elem) == 0 && n++ == instance) {
            return dev;
        }
    }

    n = 0;
    QLIST_FOREACH(dev, &bus->children, sibling) {
        if (dev->info->alias && strcmp(dev->info->alias, elem) == 0 &&
            n++ == instance) {
            return dev;
        }
    }
    return NULL;

    /*
     * try to match in order:
     *   (1) instance id, if present
     *   (2) driver name
     *   (3) driver alias, if present
     */
    /*
    QLIST_FOREACH(dev, &bus->children, sibling) {
        if (dev->id  &&  strcmp(dev->id, elem) == 0) {
            return dev;
        }
    }
    QLIST_FOREACH(dev, &bus->children, sibling) {
        if (strcmp(dev->info->name, elem) == 0) {
            return dev;
        }
    }
    QLIST_FOREACH(dev, &bus->children, sibling) {
        if (dev->info->alias && strcmp(dev->info->alias, elem) == 0) {
            return dev;
        }
    }
    return NULL;
    */
}

BusState *qbus_find(const char *path)
{
    DeviceState *dev;
    BusState *bus;
    char elem[128];
    int pos, len;

    /* find start element */
    if (path[0] == '/') {
        bus = main_system_bus;
        pos = 0;
    } else {
        if (sscanf(path, "%127[^/]%n", elem, &len) != 1) {
            assert(!path[0]);
            elem[0] = len = 0;
        }
        bus = qbus_find_recursive(main_system_bus, elem, NULL);
        if (!bus) {
            qerror_report(QERR_BUS_NOT_FOUND, elem);
            return NULL;
        }
        pos = len;
    }

    for (;;) {
        assert(path[pos] == '/' || !path[pos]);
        while (path[pos] == '/') {
            pos++;
        }
        if (path[pos] == '\0') {
            return bus;
        }

        /* find device */
        if (sscanf(path+pos, "%127[^/]%n", elem, &len) != 1) {
            assert(0);
            elem[0] = len = 0;
        }
        pos += len;
        dev = qbus_find_dev(bus, elem);
        if (!dev) {
            qerror_report(QERR_DEVICE_NOT_FOUND, elem);
            if (!monitor_cur_is_qmp()) {
                qbus_list_dev(bus);
            }
            return NULL;
        }

        assert(path[pos] == '/' || !path[pos]);
        while (path[pos] == '/') {
            pos++;
        }
        if (path[pos] == '\0') {
            /* last specified element is a device.  If it has exactly
             * one child bus accept it nevertheless */
            switch (dev->num_child_bus) {
            case 0:
                qerror_report(QERR_DEVICE_NO_BUS, elem);
                return NULL;
            case 1:
                return QLIST_FIRST(&dev->child_bus);
            default:
                qerror_report(QERR_DEVICE_MULTIPLE_BUSSES, elem);
                if (!monitor_cur_is_qmp()) {
                    qbus_list_bus(dev);
                }
                return NULL;
            }
        }

        /* find bus */
        if (sscanf(path+pos, "%127[^/]%n", elem, &len) != 1) {
            assert(0);
            elem[0] = len = 0;
        }
        pos += len;
        bus = qbus_find_bus(dev, elem);
        if (!bus) {
            qerror_report(QERR_BUS_NOT_FOUND, elem);
            if (!monitor_cur_is_qmp()) {
                qbus_list_bus(dev);
            }
            return NULL;
        }
    }
}

void qbus_create_inplace(BusState *bus, BusInfo *info,
                         DeviceState *parent, const char *name)
{
    char *buf;
    int i,len;

    bus->info = info;
    bus->parent = parent;

    if (name) {
        /* use supplied name */
        bus->name = qemu_strdup(name);
    } else if (parent && parent->id) {
        /* parent device has id -> use it for bus name */
        len = strlen(parent->id) + 16;
        buf = qemu_malloc(len);
        snprintf(buf, len, "%s.%d", parent->id, parent->num_child_bus);
        bus->name = buf;
    } else {
        /* no id -> use lowercase bus type for bus name */
        len = strlen(info->name) + 16;
        buf = qemu_malloc(len);
        len = snprintf(buf, len, "%s.%d", info->name,
                       parent ? parent->num_child_bus : 0);
        for (i = 0; i < len; i++)
            buf[i] = qemu_tolower(buf[i]);
        bus->name = buf;
    }

    QLIST_INIT(&bus->children);
    if (parent) {
        QLIST_INSERT_HEAD(&parent->child_bus, bus, sibling);
        parent->num_child_bus++;
    } else if (bus != main_system_bus) {
        /* TODO: once all bus devices are qdevified,
           only reset handler for main_system_bus should be registered here. */
        qemu_register_reset(qbus_reset_all_fn, bus);
    }
}

BusState *qbus_create(BusInfo *info, DeviceState *parent, const char *name)
{
    BusState *bus;

    bus = qemu_mallocz(info->size);
    bus->qdev_allocated = 1;
    qbus_create_inplace(bus, info, parent, name);
    return bus;
}

void qbus_free(BusState *bus)
{
    DeviceState *dev;

    while ((dev = QLIST_FIRST(&bus->children)) != NULL) {
        qdev_free(dev);
    }
    if (bus->parent) {
        QLIST_REMOVE(bus, sibling);
        bus->parent->num_child_bus--;
    } else {
        assert(bus != main_system_bus); /* main_system_bus is never freed */
        qemu_unregister_reset(qbus_reset_all_fn, bus);
    }
    qemu_free((void*)bus->name);
    if (bus->qdev_allocated) {
        qemu_free(bus);
    }
}

#define qdev_printf(fmt, ...) monitor_printf(mon, "%*s" fmt, indent, "", ## __VA_ARGS__)
static void qbus_print(Monitor *mon, BusState *bus, int indent);

static void qdev_print_props(Monitor *mon, DeviceState *dev, Property *props,
                             const char *prefix, int indent)
{
    char buf[64];

    if (!props)
        return;
    while (props->name) {
        /*
         * TODO Properties without a print method are just for dirty
         * hacks.  qdev_prop_ptr is the only such PropertyInfo.  It's
         * marked for removal.  The test props->info->print should be
         * removed along with it.
         */
        if (props->info->print) {
            props->info->print(dev, props, buf, sizeof(buf));
            qdev_printf("%s-prop: %s = %s\n", prefix, props->name, buf);
        }
        props++;
    }
}

static void qdev_print(Monitor *mon, DeviceState *dev, int indent)
{
    BusState *child;
    qdev_printf("dev: %s, id \"%s\"\n", dev->info->name,
                dev->id ? dev->id : "");
    indent += 2;
    if (dev->num_gpio_in) {
        qdev_printf("gpio-in %d\n", dev->num_gpio_in);
    }
    if (dev->num_gpio_out) {
        qdev_printf("gpio-out %d\n", dev->num_gpio_out);
    }
    qdev_print_props(mon, dev, dev->info->props, "dev", indent);
    qdev_print_props(mon, dev, dev->parent_bus->info->props, "bus", indent);
    if (dev->parent_bus->info->print_dev)
        dev->parent_bus->info->print_dev(mon, dev, indent);
    QLIST_FOREACH(child, &dev->child_bus, sibling) {
        qbus_print(mon, child, indent);
    }
}

static void qbus_print(Monitor *mon, BusState *bus, int indent)
{
    struct DeviceState *dev;

    qdev_printf("bus: %s\n", bus->name);
    indent += 2;
    qdev_printf("type %s\n", bus->info->name);
    QLIST_FOREACH(dev, &bus->children, sibling) {
        qdev_print(mon, dev, indent);
    }
}
#undef qdev_printf

void do_info_qtree(Monitor *mon)
{
    if (main_system_bus)
        qbus_print(mon, main_system_bus, 0);
}

void do_info_qdm(Monitor *mon)
{
    DeviceInfo *info;

    for (info = device_info_list; info != NULL; info = info->next) {
        qdev_print_devinfo(info);
    }
}

int do_device_add(Monitor *mon, const QDict *qdict, QObject **ret_data)
{
    QemuOpts *opts;

    opts = qemu_opts_from_qdict(qemu_find_opts("device"), qdict);
    if (!opts) {
        return -1;
    }
    if (!monitor_cur_is_qmp() && qdev_device_help(opts)) {
        qemu_opts_del(opts);
        return 0;
    }
    if (!qdev_device_add(opts)) {
        qemu_opts_del(opts);
        return -1;
    }
    return 0;
}

int do_device_del(Monitor *mon, const QDict *qdict, QObject **ret_data)
{
    const char *id = qdict_get_str(qdict, "id");
    DeviceState *dev;

    dev = qdev_find_recursive(main_system_bus, id);
    if (NULL == dev) {
        qerror_report(QERR_DEVICE_NOT_FOUND, id);
        return -1;
    }
    return qdev_unplug(dev);
}

static int qdev_get_fw_dev_path_helper(DeviceState *dev, char *p, int size)
{
    int l = 0;

    if (dev && dev->parent_bus) {
        char *d;
        l = qdev_get_fw_dev_path_helper(dev->parent_bus->parent, p, size);
        if (dev->parent_bus->info->get_fw_dev_path) {
            d = dev->parent_bus->info->get_fw_dev_path(dev);
            l += snprintf(p + l, size - l, "%s", d);
            qemu_free(d);
        } else {
            l += snprintf(p + l, size - l, "%s", dev->info->name);
        }
    }
    l += snprintf(p + l , size - l, "/");

    return l;
}

char* qdev_get_fw_dev_path(DeviceState *dev)
{
    char path[128];
    int l;

    l = qdev_get_fw_dev_path_helper(dev, path, 128);

    path[l-1] = '\0';

    return strdup(path);
}

void *qdev_iterate_recursive(BusState *bus, qdev_iteratefn callback,
                             void *opaque)
{
    DeviceState *dev, *ret;
    BusState *child;

    if (!bus) {
        bus = main_system_bus;
    }
    QLIST_FOREACH(dev, &bus->children, sibling) {
        ret = callback(dev, opaque);
        if (ret) {
            return ret;
        }
        QLIST_FOREACH(child, &dev->child_bus, sibling) {
            ret = qdev_iterate_recursive(child, callback, opaque);
            if (ret) {
                return ret;
            }
        }
    }
    return NULL;
}

static void *find_id_callback(DeviceState *dev, void *opaque)
{
    const char *id = opaque;

    if (dev->id && strcmp(dev->id, id) == 0) {
        return dev;
    }
    return NULL;
}

static DeviceState *qdev_find_id_recursive(BusState *bus, const char *id)
{
    return qdev_iterate_recursive(bus, find_id_callback, (void *)id);
}

DeviceState *qdev_find(const char *path, bool report_errors)
{
    char *dev_name;
    DeviceState *dev;
    char *bus_path;
    BusState *bus;
 
    //printf ("\n1. Within qdev_find(%s)\n", path);
    /* search for unique ID recursively if path is not absolute */
    if (path[0] != '/') {
        dev = qdev_find_id_recursive(main_system_bus, path);
        if (!dev && report_errors) {
            //printf ("1.1 not found dev: Within qdev_find(%s)\n", path);
            qerror_report(QERR_DEVICE_NOT_FOUND, path);
        }
        //printf ("1.1 found dev: Within qdev_find(%s)\n", path);
        return dev;
    }

    dev_name = strrchr(path, '/') + 1;

    bus_path = qemu_strdup(path);
    bus_path[dev_name - path] = 0;

    bus = qbus_find(bus_path);
    qemu_free(bus_path);
    if (!bus) {
        if (report_errors) {
            /* retry with full path to generate correct error message */
            bus = qbus_find(path);
        }
        if (!bus) {
            return NULL;
        }
        dev_name = (char *)"";
    }

    dev = qbus_find_dev(bus, dev_name);
    if (!dev && report_errors) {
        qerror_report(QERR_DEVICE_NOT_FOUND, dev_name);
        qbus_list_dev(bus);
    }
    return dev;
}

int qdev_instance_no(DeviceState *dev)
{
    struct DeviceState *sibling;
    int instance = 0;

    QLIST_FOREACH(sibling, &dev->parent_bus->children, sibling) {
        if (sibling->info == dev->info) {
            if (sibling == dev) {
                break;
            }
            instance++;
        }
    }
    return instance;
}

#define NAME_COLUMN_WIDTH 23

static void print_field(Monitor *mon, const QDict *qfield, int indent);

static void print_elem(Monitor *mon, const QObject *qelem, size_t size,
                       int column_pos, int indent)
{
    int64_t data_size;
    const void *data;
    int n;

    if (qobject_type(qelem) == QTYPE_QDICT) {
        if (column_pos >= 0) {
            monitor_printf(mon, ".\n");
        }
    } else {
        monitor_printf(mon, ":");
        column_pos++;
        if (column_pos < NAME_COLUMN_WIDTH) {
            monitor_printf(mon, "%*c", NAME_COLUMN_WIDTH - column_pos, ' ');
        }
    }

    switch (qobject_type(qelem)) {
    case QTYPE_QDICT:
        print_field(mon, qobject_to_qdict(qelem), indent + 2);
        break;
    case QTYPE_QBUFFER:
        data = qbuffer_get_data(qobject_to_qbuffer(qelem));
        data_size = qbuffer_get_size(qobject_to_qbuffer(qelem));
        for (n = 0; n < data_size; ) {
            monitor_printf(mon, " %02x", *((uint8_t *)data+n));
            if (++n < size) {
                if (n % 16 == 0) {
                    monitor_printf(mon, "\n%*c", NAME_COLUMN_WIDTH, ' ');
                } else if (n % 8 == 0) {
                    monitor_printf(mon, " -");
                }
            }
        }
        if (data_size < size) {
            monitor_printf(mon, " ...");
        }
        monitor_printf(mon, "\n");
        break;
    case QTYPE_QINT:
        monitor_printf(mon, "%0*" PRIx64 "\n", (int)size * 2,
                       qint_get_int(qobject_to_qint(qelem)));
        break;
    default:
        assert(0);
    }
}

static void print_field(Monitor *mon, const QDict *qfield, int indent)
{
    const char *name = qdict_get_str(qfield, "name");
    // XXX: See fixme below
    // const char *start = qdict_get_try_str(qfield, "start");
    int64_t size = qdict_get_int(qfield, "size");
    QList *qlist = qdict_get_qlist(qfield, "elems");
    QListEntry *entry, *sub_entry;
    QList *sub_list;
    int elem_no = 0;

    QLIST_FOREACH_ENTRY(qlist, entry) {
        QObject *qelem = qlist_entry_obj(entry);
        int pos = indent + strlen(name);

        if (qobject_type(qelem) == QTYPE_QLIST) {
            monitor_printf(mon, "%*c%s", indent, ' ', name);
            // XXX: monitor_printf() returns void, need to fix this
            /*
            if (start) {
                pos += monitor_printf(mon, "[%s+%02x]", start, elem_no);
            } else {
                pos += monitor_printf(mon, "[%02x]", elem_no);
            }*/
            sub_list = qobject_to_qlist(qelem);
            QLIST_FOREACH_ENTRY(sub_list, sub_entry) {
                print_elem(mon, qlist_entry_obj(sub_entry), size, pos,
                           indent + 2);
                pos = -1;
            }
        } else {
            if (elem_no == 0) {
                monitor_printf(mon, "%*c%s", indent, ' ', name);
            } else {
                pos = -1;
            }
            print_elem(mon, qelem, size, pos, indent);
        }
        elem_no++;
    }
}

void device_user_print(Monitor *mon, const QObject *data)
{
    QDict *qdict = qobject_to_qdict(data);
    QList *qlist = qdict_get_qlist(qdict, "fields");
    QListEntry *entry;

    monitor_printf(mon, "dev: %s, id \"%s\", version %" PRId64 "\n",
                   qdict_get_str(qdict, "device"),
                   qdict_get_str(qdict, "id"),
                   qdict_get_int(qdict, "version"));

    QLIST_FOREACH_ENTRY(qlist, entry) {
        print_field(mon, qobject_to_qdict(qlist_entry_obj(entry)), 2);
    }
}

static size_t parse_vmstate(const VMStateDescription *vmsd, void *opaque,
                            QList *qlist, int full_buffers)
{
    VMStateField *field;
    size_t overall_size = 0;

    field = vmsd->fields;
    if (vmsd->pre_save) {
        vmsd->pre_save(opaque);
    }
    while(field->name) {
        if (!field->field_exists ||
            field->field_exists(opaque, vmsd->version_id)) {
            void *base_addr = opaque + field->offset;
            int i, n_elems = 1;
            int is_array = 1;
            size_t size = field->size;
            size_t real_size = 0;
            size_t dump_size;
            QDict *qfield = qdict_new();
            QList *qelems = qlist_new();

            qlist_append_obj(qlist, QOBJECT(qfield));

            if (field->flags & (VMS_BITFIELD))
            {
              field->name = field->bit_field_name;
            }

            qdict_put_obj(qfield, "name",
                          QOBJECT(qstring_from_str(field->name)));
            qdict_put_obj(qfield, "elems", QOBJECT(qelems));

            if (field->flags & VMS_VBUFFER) {
                size = *(int32_t *)(opaque + field->size_offset);
                if (field->flags & VMS_MULTIPLY) {
                    size *= field->size;
                }
            }
            if (field->start_index) {
                qdict_put_obj(qfield, "start",
                              QOBJECT(qstring_from_str(field->start_index)));
            }

            printf ("\nName: %s, field->offset: %d", field->name, field->offset);
            if (field->flags & VMS_ARRAY) {
                n_elems = field->num;
            } else if (field->flags & VMS_VARRAY_INT32) {
                n_elems = *(int32_t *)(opaque + field->num_offset);
            } else if (field->flags & VMS_VARRAY_UINT16) {
                n_elems = *(uint16_t *)(opaque + field->num_offset);
            } else {
                is_array = 0;
            }
            if (field->flags & VMS_POINTER) {
                base_addr = *(void **)base_addr + field->start;
            }
            for (i = 0; i < n_elems; i++) {
                void *addr = base_addr + size * i;
                QList *sub_elems = qelems;
                int val;

                if (is_array) {
                    sub_elems = qlist_new();
                    qlist_append_obj(qelems, QOBJECT(sub_elems));
                }
                if (field->flags & VMS_ARRAY_OF_POINTER) {
                    addr = *(void **)addr;
                }
                if (field->flags & VMS_STRUCT) {
                    real_size = parse_vmstate(field->vmsd, addr,
                                              sub_elems, full_buffers);
                } else {
                    real_size = size;
                    if (field->flags & (VMS_BUFFER | VMS_VBUFFER)) {
                        dump_size = (full_buffers || size <= 16) ? size : 16;
                        qlist_append_obj(sub_elems,
                                QOBJECT(qbuffer_from_data(addr, dump_size)));
                    } else {

                        if (field->flags & VMS_QUEUE)
                        {
                          //QLIST_FOREACH_ENTRY(field->name, entry) {
                            field->queue_print_cb ((void *) addr);  
//                          }
                        }
                        else
                        {
                          switch (size) {
                            case 1:
                              val = *(uint8_t *)addr;
                              break;
                            case 2:
                              val = *(uint16_t *)addr;
                              break;
                            case 4:
                              val = *(uint32_t *)addr;
                              break;
                            case 8:
                              val = *(uint64_t *)addr;
                              break;
                            default:
                              assert(0);
                          }
                          
                          // If it's a bitfield, we apply the
                          // mask on the value and only specify
                          // if the bit is set or not.
                          if (field->flags & (VMS_BITFIELD))
                            {
                              val = val & (field->bit_field_mask);
                              val = !(val == 0);
                            }
   
                          qlist_append_obj(sub_elems,
                                           QOBJECT(qint_from_int(val)));
                        }
                    }
                }
                overall_size += real_size;
            }
            qdict_put_obj(qfield, "size", QOBJECT(qint_from_int(real_size)));
        }
        field++;
    }
    return overall_size;
}


int do_device_show(Monitor *mon, const QDict *qdict, QObject **ret_data)
{
    const char *path = qdict_get_str(qdict, "path");
    const VMStateDescription *vmsd;
    DeviceState *dev;
    QList *qlist;
    int name_len;
    char *name;

    dev = qdev_find(path, true);
    if (!dev) {
        return -1;
    }

    vmsd = dev->info->vmsd;
    if (!vmsd) {
        qerror_report(QERR_DEVICE_NO_STATE, dev->info->name);
        error_printf_unless_qmp("Note: device may simply lack complete qdev "
                                "conversion\n");
        return -1;
    }

    name_len = strlen(dev->info->name) + 16;
    name = qemu_malloc(name_len);
    snprintf(name, name_len, "%s.%d", dev->info->name, qdev_instance_no(dev));
    *ret_data = qobject_from_jsonf("{ 'device': %s, 'id': %s, 'version': %d }",
                                   name, dev->id ? : "", vmsd->version_id);
    qemu_free(name);
    qlist = qlist_new();
    parse_vmstate(vmsd, dev, qlist, 0 /*qdict_get_int(qdict, "full")*/);
    qdict_put_obj(qobject_to_qdict(*ret_data), "fields", QOBJECT(qlist));

    return 0;
}
