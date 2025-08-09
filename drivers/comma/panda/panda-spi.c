#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/of.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/poll.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/string.h>

#define SPI_CHECKSUM_START   0xAB

struct panda_spi {
    struct spi_device *spi;
    struct mutex lock; // protects reconfiguration and SPI xfers

    // Streaming artifacts removed; synchronous I/O only

    // ACK/retry config for write()
    bool ack_enable;
    u8   ack_value;          // expected ack byte
    u32  ack_retries;
    u32  ack_delay_us;       // delay between write and ack read

    // Char device
    struct miscdevice miscdev;
    char *misc_name;

    // Normal transaction state
    bool header_valid;      // last header accepted (HACK received)
    u16 exp_mosi_len;       // payload bytes host will write
    u16 exp_miso_len;       // max payload bytes device may return
    bool dack_pending;      // expect a DACK frame on next read
};
// XOR checksum helper with seed 0xAB as per protocol
static u8 panda_xor_chk(const u8 *data, size_t len)
{
    u8 chk = SPI_CHECKSUM_START;
    size_t i;
    for (i = 0; i < len; i++)
        chk ^= data[i];
    return chk;
}


// (checksum helpers implemented as panda_xor_chk)

static ssize_t ack_enable_show(struct device *dev,
                               struct device_attribute *attr, char *buf)
{
    struct panda_spi *ps = spi_get_drvdata(to_spi_device(dev));
    return scnprintf(buf, PAGE_SIZE, "%u\n", ps->ack_enable);
}

static ssize_t ack_enable_store(struct device *dev,
                                struct device_attribute *attr,
                                const char *buf, size_t count)
{
    unsigned int v; int ret;
    struct panda_spi *ps = spi_get_drvdata(to_spi_device(dev));
    ret = kstrtouint(buf, 0, &v);
    if (ret) {
        return ret;
    }
    ps->ack_enable = !!v;
    return count;
}

static DEVICE_ATTR(ack_enable, 0644, ack_enable_show, ack_enable_store);

static ssize_t ack_value_show(struct device *dev,
                              struct device_attribute *attr, char *buf)
{
    struct panda_spi *ps = spi_get_drvdata(to_spi_device(dev));
    return scnprintf(buf, PAGE_SIZE, "0x%02x\n", ps->ack_value);
}
static ssize_t ack_value_store(struct device *dev,
                               struct device_attribute *attr,
                               const char *buf, size_t count)
{
    unsigned int v; int ret;
    struct panda_spi *ps = spi_get_drvdata(to_spi_device(dev));
    ret = kstrtouint(buf, 0, &v);
    if (ret) {
        return ret;
    }
    ps->ack_value = (u8)v;
    return count;
}
static DEVICE_ATTR(ack_value, 0644, ack_value_show, ack_value_store);

static ssize_t ack_retries_show(struct device *dev,
                                struct device_attribute *attr, char *buf)
{
    struct panda_spi *ps = spi_get_drvdata(to_spi_device(dev));
    return scnprintf(buf, PAGE_SIZE, "%u\n", ps->ack_retries);
}
static ssize_t ack_retries_store(struct device *dev,
                                 struct device_attribute *attr,
                                 const char *buf, size_t count)
{
    unsigned int v; int ret;
    struct panda_spi *ps = spi_get_drvdata(to_spi_device(dev));
    ret = kstrtouint(buf, 0, &v);
    if (ret){
        return ret;
    }
    if (v > 1000) {
        return -EINVAL;
    }
    ps->ack_retries = v;
    return count;
}
static DEVICE_ATTR(ack_retries, 0644, ack_retries_show, ack_retries_store);

static ssize_t ack_delay_us_show(struct device *dev,
                                 struct device_attribute *attr, char *buf)
{
    struct panda_spi *ps = spi_get_drvdata(to_spi_device(dev));
    return scnprintf(buf, PAGE_SIZE, "%u\n", ps->ack_delay_us);
}
static ssize_t ack_delay_us_store(struct device *dev,
                                  struct device_attribute *attr,
                                  const char *buf, size_t count)
{
    unsigned int v; int ret;
    struct panda_spi *ps = spi_get_drvdata(to_spi_device(dev));
    ret = kstrtouint(buf, 0, &v);
    if (ret) {
        dev_err(&ps->spi->dev, "panda_chr_write invalid ack_delay_us: %u\n", v);
        return ret;
    }
    if (v > 1000000) {
        dev_err(&ps->spi->dev,"panda_chr_write invalid ack_delay_us: %u\n", v);
        return -EINVAL;
    }
    ps->ack_delay_us = v;
    return count;
}
static DEVICE_ATTR(ack_delay_us, 0644, ack_delay_us_show, ack_delay_us_store);


static struct attribute *panda_ack_attrs[] = {
    &dev_attr_ack_enable.attr,
    &dev_attr_ack_value.attr,
    &dev_attr_ack_retries.attr,
    &dev_attr_ack_delay_us.attr,
    NULL,
};

static const struct attribute_group panda_ack_attr_group = {
    .name = "ack",
    .attrs = panda_ack_attrs,
};

// ===================== Char device I/O =====================
static ssize_t panda_chr_read(struct file *filp, char __user *buf, size_t count, loff_t *ppos)
{
    struct panda_spi *ps = filp->private_data;
    int ret = 0;
    u8 *tx = NULL, *rx = NULL;

    if (!ps || !ps->spi) {
        dev_err(&ps->spi->dev, "panda_chr_read called with no SPI device\n");
        return -ENODEV;
    }
    if (count == 0) {
        dev_info(&ps->spi->dev, "panda_chr_read called with zero count\n");
        return 0;
    }

    // If expecting DACK, read full framed response and validate XOR checksum
    if (ps->dack_pending) {
        dev_info(&ps->spi->dev, "panda_chr_read expecting DACK response\n");
        size_t read_len = 4 + ps->exp_miso_len; // DACK + length + payload + checksum + padding
        u16 resp_len;
        size_t out_len;
        if (count < read_len) {
            dev_err(&ps->spi->dev,"panda_chr_read buffer too small\n");
            return -EINVAL;
        }
        tx = kmalloc(read_len, GFP_KERNEL);
        rx = kmalloc(read_len, GFP_KERNEL);
        if (!tx || !rx) {
            dev_err(&ps->spi->dev,"panda_chr_read failed to allocate memory\n");
            ret = -ENOMEM;
            goto out_free;
        }
        memset(tx, 0xFF, read_len);
        mutex_lock(&ps->lock);
        {
            dev_info(&ps->spi->dev,"panda_chr_read sending SPI transfer\n");
            struct spi_transfer xfer = { .tx_buf = tx, .rx_buf = rx, .len = read_len };
            struct spi_message m; spi_message_init(&m); spi_message_add_tail(&xfer, &m);
            ret = spi_sync(ps->spi, &m);
            dev_info(&ps->spi->dev,"panda_chr_read SPI transfer completed\n");
        }
        mutex_unlock(&ps->lock);
        // Clear pending regardless
        ps->dack_pending = false;
        if (ret) {
            dev_err(&ps->spi->dev,"panda_chr_read SPI transfer failed: %d\n", ret);
            goto out_free;
        }

        // Handle NACK
        if (rx[0] == 0x1F) {
            dev_err(&ps->spi->dev,"panda_chr_read NACK received\n");
            ret = -EIO;
            goto out_free;
        }
        // Must start with DACK
        if (rx[0] != 0x85) {
            dev_err(&ps->spi->dev,"panda_chr_read invalid response: 0x%02X\n", rx[0]);
            ret = -EPROTO;
            goto out_free;
        }

        resp_len = rx[1] | (rx[2] << 8);
        if (resp_len > ps->exp_miso_len) {
            dev_err(&ps->spi->dev,"panda_chr_read unexpected response length: %zu > %zu\n",
                    resp_len, ps->exp_miso_len);
            ret = -EPROTO;
            goto out_free;
        }

        // Verify checksum over bytes [0..(2+resp_len)] with seed 0xAB; compare to rx[3+resp_len]
        if (panda_xor_chk(rx, 3 + resp_len) != rx[3 + resp_len]) {
            dev_err(&ps->spi->dev,"panda_chr_read checksum mismatch\n");
            ret = -EBADMSG;
            goto out_free;
        }

        // Return only compact frame (DACK + len + resp payload + checksum), ignore padding
        out_len = 4 + resp_len;
        if (copy_to_user(buf, rx, out_len)) {
            dev_err(&ps->spi->dev,"panda_chr_read failed to copy to user\n");
            ret = -EFAULT;
            goto out_free;
        }
        dev_info(&ps->spi->dev,"panda_chr_read DACK response read successfully. ret = %zu\n", out_len);
        ret = out_len;
    } else {
        dev_info(&ps->spi->dev,"panda_chr_read sending SPI transfer\n");
        // If a header was accepted but data phase not yet written, don't clock MISO
        if (ps->header_valid) {
            dev_err(&ps->spi->dev,"panda_chr_read data phase not yet written\n");
            return -EBUSY;
        }
        // Pass-through read: clock exactly count bytes
        tx = kmalloc(count, GFP_KERNEL);
        rx = kmalloc(count, GFP_KERNEL);
        if (!tx || !rx) {
            ret = -ENOMEM;
            goto out_free;
        }
        memset(tx, 0xFF, count);
        mutex_lock(&ps->lock);
        {
            dev_info(&ps->spi->dev,"panda_chr_read sending SPI transfer\n");
            struct spi_transfer xfer = { .tx_buf = tx, .rx_buf = rx, .len = count };
            struct spi_message m; spi_message_init(&m); spi_message_add_tail(&xfer, &m);
            ret = spi_sync(ps->spi, &m);
        }
        mutex_unlock(&ps->lock);
        if (ret) {
            dev_err(&ps->spi->dev,"panda_chr_read SPI transfer failed: %d\n", ret);
            goto out_free;
        }
        if (copy_to_user(buf, rx, count)) {
            dev_err(&ps->spi->dev,"panda_chr_read failed to copy to user\n");
            ret = -EFAULT;
            goto out_free;
        }
        dev_info(&ps->spi->dev,"panda_chr_read DACK response read successfully. ret = %zu\n", count);
        ret = count;
    }
out_free:
    kfree(tx);
    kfree(rx);
    return ret;
}

static ssize_t panda_chr_write(struct file *filp, const char __user *buf, size_t count, loff_t *ppos)
{
    struct panda_spi *ps = filp->private_data;
    int ret = 0;
    u8 *kbuf;
    u8 ack = 0;
    unsigned int i;

    if (!ps || !ps->spi) {
        return -ENODEV;
    }
    dev_info(&ps->spi->dev, "panda_chr_write called with %zu bytes\n", count);

    if (count == 0) {
        return 0;
    }
    kbuf = kmalloc(count, GFP_KERNEL);
    if (!kbuf) {
        return -ENOMEM;
    }
    if (copy_from_user(kbuf, buf, count)) {
        kfree(kbuf);
        return -EFAULT;
    }

    if (count == 7 && kbuf[0] == 0x5A) {
        dev_info(&ps->spi->dev, "panda_chr_write header received\n");
        // Normal header path: validate XOR checksum before sending
        u8 chk = panda_xor_chk(kbuf, 6);
        u8 header_chk = kbuf[6];
        u8 ackb = 0;
        if (chk != header_chk) {
            dev_err(&ps->spi->dev,"panda_chr_write header checksum mismatch\n");
            kfree(kbuf);
            return -EINVAL;
        }

        // Send header
        mutex_lock(&ps->lock);
        {
            dev_info(&ps->spi->dev,"panda_chr_write sending header\n");
            struct spi_transfer xh = { .tx_buf = kbuf, .len = 7 };
            struct spi_message mh; spi_message_init(&mh); spi_message_add_tail(&xh, &mh);
            ret = spi_sync(ps->spi, &mh);
            if (ret) {
                dev_err(&ps->spi->dev,"panda_chr_write header send failed\n");
                mutex_unlock(&ps->lock);
                kfree(kbuf);
                return ret; 
            }
            // Read one ACK/NACK byte
            dev_info(&ps->spi->dev,"panda_chr_write waiting for ACK/NACK\n");
            ret = spi_read(ps->spi, &ackb, 1);
        }
        mutex_unlock(&ps->lock);
        if (ret) {
            dev_err(&ps->spi->dev,"panda_chr_write waiting for ACK/NACK failed\n");
            kfree(kbuf);
            return ret;
        }
        if (ackb != 0x79) { // HACK expected
            dev_err(&ps->spi->dev,"panda_chr_write unexpected ACK/NACK value. got 0x%02X, expected 0x79\n", ackb);
            kfree(kbuf);
            return -EIO;
        }
        // Header accepted; track lengths for data and expected DACK size
        ps->exp_mosi_len = kbuf[2] | (kbuf[3] << 8);
        ps->exp_miso_len = kbuf[4] | (kbuf[5] << 8);
        ps->header_valid = true;
        kfree(kbuf);
        dev_info(&ps->spi->dev,"panda_chr_write header accepted: exp_mosi_len=%u, exp_miso_len=%u\n",
                 ps->exp_mosi_len, ps->exp_miso_len);
        dev_info(&ps->spi->dev,"panda_chr_write waiting for data phase\n");
        return count;
    } else if (ps->header_valid) {
        dev_info(&ps->spi->dev,"panda_chr_write data phase received\n");
        // Data phase write: expect payload + 1 checksum byte
        u16 expect = ps->exp_mosi_len;
        u8 chk;
        if (count != (size_t)(expect + 1)) {
            dev_err(&ps->spi->dev,"panda_chr_write data length mismatch: got %zu, expected %u+1\n",
                    count, expect);
            // Abort transaction locally to allow recovery
            ps->header_valid = false;
            kfree(kbuf);
            return -EINVAL;
        }
        chk = panda_xor_chk(kbuf, expect);
        if (chk != kbuf[expect]) {
            dev_err(&ps->spi->dev,"panda_chr_write data checksum mismatch: got 0x%02X, expected 0x%02X\n",
                    kbuf[expect], chk);
            // Abort transaction locally to allow recovery
            ps->header_valid = false;
            kfree(kbuf);
            return -EINVAL;
        }

        // Send payload + checksum
        mutex_lock(&ps->lock);
        {
            dev_info(&ps->spi->dev,"panda_chr_write sending data\n");
            struct spi_transfer xd = { .tx_buf = kbuf, .len = count };
            struct spi_message md; spi_message_init(&md); spi_message_add_tail(&xd, &md);
            ret = spi_sync(ps->spi, &md);
        }
        mutex_unlock(&ps->lock);
        if (ret) {
            kfree(kbuf);
            return ret;
        }
        // Expect DACK on next read
        dev_info(&ps->spi->dev,"panda_chr_write data sent successfully, waiting for DACK\n");
        ps->header_valid = false;
        ps->dack_pending = true;
        kfree(kbuf);
        return count;
    }

    // Prevent arbitrary writes while mid-transaction
    if (ps->header_valid || ps->dack_pending) {
        dev_warn(&ps->spi->dev,"panda_chr_write busy\n");
        kfree(kbuf);
        return -EBUSY;
    }

    mutex_lock(&ps->lock);

    // Transmit payload
    {
        dev_info(&ps->spi->dev,"panda_chr_write sending data\n");
        struct spi_transfer xfer = {
            .tx_buf = kbuf,
            .len = count,
        };
        struct spi_message m;
        spi_message_init(&m);
        spi_message_add_tail(&xfer, &m);
        ret = spi_sync(ps->spi, &m);
        if (ret) {
            dev_err(&ps->spi->dev,"panda_chr_write data send failed\n");
            goto out_unlock;
        }
    }

    // Optional ACK handling (block for ACK byte if enabled)
    if (ps->ack_enable) {
        for (i = 0; i <= ps->ack_retries; i++) {
            int rret;
            if (ps->ack_delay_us) {
                udelay(min(ps->ack_delay_us, 2000u));
            }

            rret = spi_read(ps->spi, &ack, 1);
            if (rret) {
                dev_err(&ps->spi->dev,"panda_chr_write ACK read failed\n");
                ret = rret;
                continue;
            }
            if (ack == ps->ack_value) {
                dev_info(&ps->spi->dev,"panda_chr_write ACK received\n");
                ret = count;
                break;
            }
            dev_warn(&ps->spi->dev,"panda_chr_write unexpected ACK: 0x%02X\n", ack);
            ret = -EIO; // unexpected ack value
        }
        goto out_unlock;
    }
    dev_info(&ps->spi->dev,"panda_chr_write ACK handling disabled, skipping\n");

    ret = count;

out_unlock:
    mutex_unlock(&ps->lock);
    kfree(kbuf);
    return ret;
}

static int panda_chr_open(struct inode *inode, struct file *filp)
{
    struct miscdevice *m = filp->private_data;   // set by misc core
    struct panda_spi *ps;

    if (!m) {
        return -ENODEV;
    }
    // Recover our parent struct safely
    ps = container_of(m, struct panda_spi, miscdev);
    if (!ps || !ps->spi) {
        return -ENODEV;
    }
    // Reset protocol state on each open to recover from previous errors
    ps->header_valid = false;
    ps->dack_pending = false;
    ps->exp_mosi_len = 0;
    ps->exp_miso_len = 0;
    filp->private_data = ps;   // hand ps to read/write
    dev_info(&ps->spi->dev, "Panda SPI device opened\n");
    return nonseekable_open(inode, filp);
}

static int panda_chr_release(struct inode *inode, struct file *filp)
{
    struct panda_spi *ps = filp->private_data;
    if (ps) {
        dev_info(&ps->spi->dev, "Panda SPI device released\n");
        // Clear protocol state on close to recover from partial transactions
        ps->header_valid = false;
        ps->dack_pending = false;
        ps->exp_mosi_len = 0;
        ps->exp_miso_len = 0;
    }
    return 0;
}

static const struct file_operations panda_fops = {
    .owner = THIS_MODULE,
    .read = panda_chr_read,
    .write = panda_chr_write,
    .open = panda_chr_open,

    .release = panda_chr_release,
    .llseek = no_llseek,
};

// ===================== Probe/Remove =====================
static int panda_spi_probe(struct spi_device *spi)
{
    int ret;
    struct panda_spi *ps;

    ps = devm_kzalloc(&spi->dev, sizeof(*ps), GFP_KERNEL);
    if (!ps) {
        dev_err(&spi->dev, "Failed to allocate panda_spi struct\n");
        return -ENOMEM;
    }

    ps->spi = spi;
    mutex_init(&ps->lock);
    spi_set_drvdata(spi, ps);


    // ACK defaults
    ps->ack_enable = false;
    ps->ack_value = 0x06; // ASCII ACK
    ps->ack_retries = 3;
    ps->ack_delay_us = 0;

    spi->bits_per_word = 8;
    spi->max_speed_hz = 50000000; // 50 MHz default
    spi->mode = SPI_MODE_0;

    ret = spi_setup(spi);
    if (ret) {
        dev_err(&spi->dev, "spi_setup failed: %d\n", ret);
        return ret;
    }

    ret = sysfs_create_group(&spi->dev.kobj, &panda_ack_attr_group);
    if (ret)
        goto err_group_ack;

    // Character device registration (per SPI device)
    ps->misc_name = kasprintf(GFP_KERNEL, "panda_spi%d.%d",
                              spi->master->bus_num, spi->chip_select);
    if (!ps->misc_name) {
        dev_err(&spi->dev, "Failed to allocate misc_name\n");
        ret = -ENOMEM;
        goto err_group_ack;
    }

    ps->miscdev.minor = MISC_DYNAMIC_MINOR;
    ps->miscdev.name = ps->misc_name;
    ps->miscdev.fops = &panda_fops;
    ps->miscdev.parent = &spi->dev;

    ret = misc_register(&ps->miscdev);
    if (ret) {
        dev_err(&spi->dev, "failed to register miscdev: %d\n", ret);
        goto err_group_ack;
    }
    // Attach driver data to the misc device for open()
    //dev_set_drvdata(ps->miscdev.this_device, ps);

    dev_info(&spi->dev, "Panda SPI device probed: mode=0x%x, bpw=%u, max_speed=%u Hz, chardev=/dev/%s\n",
             spi->mode, spi->bits_per_word, spi->max_speed_hz, ps->misc_name);
    dev_dbg(&spi->dev, "Panda SPI device debug test message\n");
    dev_err(&spi->dev, "Panda SPI device error test message\n");
    dev_alert(&spi->dev, "Panda SPI device alert test message\n");
    dev_crit(&spi->dev, "Panda SPI device critical test message\n");

    return 0;

err_group_ack:
    sysfs_remove_group(&spi->dev.kobj, &panda_ack_attr_group);
    return ret;
}

static int panda_spi_remove(struct spi_device *spi)
{
    struct panda_spi *ps = spi_get_drvdata(spi);

    misc_deregister(&ps->miscdev);
    sysfs_remove_group(&spi->dev.kobj, &panda_ack_attr_group);
    kfree(ps->misc_name);
    dev_info(&spi->dev, "Panda SPI device removed\n");
    return 0;
}

static const struct of_device_id panda_spi_of_match[] = {
    { .compatible = "commaai,panda" },
    {}
};
MODULE_DEVICE_TABLE(of, panda_spi_of_match);

static struct spi_driver panda_spi_driver = {
    .driver = {
        .name = "panda-spi",
        .of_match_table = panda_spi_of_match,
    },
    .probe = panda_spi_probe,
    .remove = panda_spi_remove,
};

module_spi_driver(panda_spi_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Comma.ai");
MODULE_DESCRIPTION("SPI protocol driver for Comma.ai Panda");