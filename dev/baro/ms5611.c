/*
 * Copyright (C) 2013, 2014 F4OS Authors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is furnished to do
 * so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <libfdt.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <dev/fdtparse.h>
#include <dev/hw/i2c.h>
#include <dev/baro.h>
#include <kernel/init.h>
#include <kernel/mutex.h>
#include <mm/mm.h>

struct ms5611 {
    uint8_t             ready;
    int                 addr;
    struct mutex        lock;
    uint16_t            c[7];   /* Conversion parameters */
};

#define MS5611_COMPAT    "meas-spec,ms5611-01ba03"

static int ms5611_init(struct baro *baro) {
    struct i2c *i2c = to_i2c(baro->device.parent);
    struct i2c_ops *i2c_ops = (struct i2c_ops *)i2c->obj.ops;
    struct ms5611 *ms5611_baro = (struct ms5611 *) baro->priv;
    int ret;

    uint8_t packet;
    uint8_t data[2];

    acquire(&ms5611_baro->lock);

    for (int i = 1; i <= 6; i++) {
        packet = 0xA0 + 2*i;  /* Read C[i] */
        ret = i2c_ops->write(i2c, ms5611_baro->addr, &packet, 1);
        if (ret != 1) {
            goto err_release_mut;
        }

        ret = i2c_ops->read(i2c, ms5611_baro->addr, data, 2);
        if (ret != 2) {
            goto err_release_mut;
        }

        ms5611_baro->c[i] = (data[0] << 8) | data[1];
    }

    release(&ms5611_baro->lock);

    ms5611_baro->ready = 1;

    return 0;

err_release_mut:
    release(&ms5611_baro->lock);
    return -1;
}

static int ms5611_deinit(struct baro *baro) {
    /* The MS5611 has no low power mode */
    return 0;
}

static int ms5611_has_temp(struct baro *baro) {
    return 1;
}

/**
 * Read MS5611 ADC
 *
 * Read the internal ADC and return its contents
 *
 * @param i2c   I2C bus that MS5611 is on
 * @param addr  Device I2C address
 * @param value Pointer to unsigned int to place ADC value in
 *
 * @returns 0 on sucess, negative on error
 */
static int ms5611_read_adc(struct i2c *i2c, uint8_t addr, uint32_t *value) {
    struct i2c_ops *i2c_ops = (struct i2c_ops *) i2c->obj.ops;
    uint8_t packet, raw_data[4];
    int ret;

    packet = 0x00;  /* Read ADC */
    ret = i2c_ops->write(i2c, addr, &packet, 1);
    if (ret != 1) {
        return -1;
    }

    ret = i2c_ops->read(i2c, addr, raw_data, 3);
    if (ret != 3) {
        return -1;
    }

    *value = (raw_data[0] << 16) | (raw_data[1] << 8) | raw_data[3];

    return 0;
}

static int ms5611_get_data(struct baro *baro, struct baro_data *data) {
    struct i2c *i2c = to_i2c(baro->device.parent);
    struct i2c_ops *i2c_ops = (struct i2c_ops *) i2c->obj.ops;
    struct ms5611 *ms5611_baro = (struct ms5611 *) baro->priv;
    uint8_t packet;
    uint32_t d1, d2;
    int ret;

    if (!ms5611_baro->ready) {
        struct baro_ops *baro_ops = (struct baro_ops *) baro->obj.ops;
        ret = baro_ops->init(baro);
        if (ret) {
            return ret;
        }
    }

    acquire(&ms5611_baro->lock);

    packet = 0x48;  /* Initiate pressure conversion */
    ret = i2c_ops->write(i2c, ms5611_baro->addr, &packet, 1);
    if (ret != 1) {
        goto err_release_mut;
    }

    /* Wait 10ms for conversion */
    usleep(10000);

    /* Digital pressure value */
    ret = ms5611_read_adc(i2c, ms5611_baro->addr, &d1);
    if (ret) {
        goto err_release_mut;
    }

    packet = 0x58;  /* Initiate temperature conversion */
    ret = i2c_ops->write(i2c, ms5611_baro->addr, &packet, 1);
    if (ret != 1) {
        goto err_release_mut;
    }

    /* Wait 10ms for conversion */
    usleep(10000);

    /* Digital temperature value */
    ret = ms5611_read_adc(i2c, ms5611_baro->addr, &d2);
    if (ret) {
        goto err_release_mut;
    }

    release(&ms5611_baro->lock);

    /* Perform conversion to actual values */
    int64_t dT = d2 - ((uint64_t) ms5611_baro->c[5] << 8);
    int64_t temp = 2000 + ((dT * (uint64_t) ms5611_baro->c[6]) >> 23);

    int64_t off = ((uint64_t) ms5611_baro->c[2] << 16) +
                  (((uint64_t) ms5611_baro->c[4] * dT) >> 7);

    int64_t sens = ((uint64_t) ms5611_baro->c[1] << 15) +
                   (((uint64_t) ms5611_baro->c[3] * dT) >> 8);

    /* Untested! It isn't cold enough in here */
    if (temp < 2000) {
        int64_t temp2;
        int64_t off2;
        int64_t sens2;

        temp2 = (dT * dT) >> 31;
        off2 = (5 * (temp - 2000) * (temp - 2000)) >> 1;
        sens2 = (5 * (temp - 2000) * (temp - 2000)) >> 2;

        if (temp < -1500) {
            off2 = off2 + 7 * (temp + 1500) * (temp + 1500);
            sens2 = sens2 + ((11 * (temp + 1500) * (temp + 1500)) >> 1);
        }

        temp = temp - temp2;
        off = off - off2;
        sens = sens - sens2;
    }

    int64_t pressure = ((d1 * sens >> 21) - off) >> 15;

    data->pressure = (float) (int) pressure;
    data->temperature = ((int) temp)/100.0;

    return 0;

err_release_mut:
    release(&ms5611_baro->lock);
    return -1;
}

struct baro_ops ms5611_ops = {
    .init = ms5611_init,
    .deinit = ms5611_deinit,
    .has_temp = ms5611_has_temp,
    .get_data = ms5611_get_data,
};

/* No way to identify chip, simply check for something at the address */
static int ms5611_probe(const char *name) {
    const void *blob = fdtparse_get_blob();
    int offset, parent_offset, addr, err;
    char *parent;
    struct obj *i2c_obj;
    struct i2c *i2c;
    struct i2c_ops *i2c_ops;
    uint8_t data;
    int ret = 0;

    offset = fdt_path_offset(blob, name);
    if (offset < 0) {
        return 0;
    }

    if (fdt_node_check_compatible(blob, offset, MS5611_COMPAT)) {
        return 0;
    }

    parent_offset = fdt_parent_offset(blob, offset);
    if (parent_offset < 0) {
        return 0;
    }

    parent = fdtparse_get_path(blob, parent_offset);
    if (!parent) {
        return 0;
    }

    i2c_obj = device_get(parent);
    if (!i2c_obj) {
        goto out_free_parent;
    }

    i2c = to_i2c(i2c_obj);
    i2c_ops = (struct i2c_ops *)i2c->obj.ops;

    err = fdtparse_get_int(blob, offset, "reg", &addr);
    if (err) {
        goto out;
    }

    /* Attempt write of dummy register */
    data = 0;
    err = i2c_ops->write(i2c, addr, &data, 1);
    if (err == 1) {
        /*
         * Either *something* is at this address, or this i2c peripheral
         * doesn't handle bus errors.
         */
        ret = 1;
    }
    else {
        ret = 0;
    }

out:
    device_put(i2c_obj);
out_free_parent:
    free(parent);
    return ret;
}

static struct obj *ms5611_ctor(const char *name) {
    const void *blob = fdtparse_get_blob();
    int offset, parent_offset, err;
    char *parent;
    struct obj *baro_obj;
    struct baro *baro;
    struct ms5611 *ms5611_baro;

    offset = fdt_path_offset(blob, name);
    if (offset < 0) {
        return NULL;
    }

    if (fdt_node_check_compatible(blob, offset, MS5611_COMPAT)) {
        return NULL;
    }

    parent_offset = fdt_parent_offset(blob, offset);
    if (parent_offset < 0) {
        return NULL;
    }

    parent = fdtparse_get_path(blob, parent_offset);
    if (!parent) {
        return NULL;
    }

    /* Instantiate an baro obj with ms5611 ops */
    baro_obj = instantiate(name, &baro_class, &ms5611_ops, struct baro);
    if (!baro_obj) {
        goto err_free_parent;
    }

    /* Connect baro to its parent I2C device */
    baro = to_baro(baro_obj);
    baro->device.parent = device_get(parent);
    if (!baro->device.parent) {
        goto err_free_obj;
    }

    /* Set up private data */
    baro->priv = kmalloc(sizeof(struct ms5611));
    if (!baro->priv) {
        goto err_free_obj;
    }

    ms5611_baro = (struct ms5611 *) baro->priv;
    ms5611_baro->ready = 0;
    init_mutex(&ms5611_baro->lock);

    err = fdtparse_get_int(blob, offset, "reg", &ms5611_baro->addr);
    if (err) {
        goto err_free_priv;
    }

    /* Export to the OS */
    class_export_member(baro_obj);

    free(parent);

    return baro_obj;

err_free_priv:
    kfree(baro->priv);
err_free_obj:
    class_deinstantiate(baro_obj);
err_free_parent:
    free(parent);
    return NULL;
}

static struct mutex ms5611_driver_mut = INIT_MUTEX;

static struct device_driver ms5611_compat_driver = {
    .name = MS5611_COMPAT,
    .probe = ms5611_probe,
    .ctor = ms5611_ctor,
    .class = &baro_class,
    .mut = &ms5611_driver_mut,
};

static int ms5611_register(void) {
    device_compat_driver_register(&ms5611_compat_driver);

    return 0;
}
CORE_INITIALIZER(ms5611_register)
