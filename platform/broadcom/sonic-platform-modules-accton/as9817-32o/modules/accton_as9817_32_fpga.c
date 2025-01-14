/*
 * Copyright (C)  Roger Ho <roger530_ho@edge-core.com>
 *
 * This module supports the accton fpga via pcie that read/write reg
 * mechanism to get OSFP/SFP status ...etc.
 * This includes the:
 *     Accton as9817_32 FPGA
 *
 * Copyright (C) 2017 Finisar Corp.
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/i2c.h>
#include <linux/mutex.h>
#include <linux/interrupt.h>
#include <linux/i2c-mux.h>
#include <linux/version.h>
#include <linux/stat.h>
#include <linux/hwmon-sysfs.h>
#include <linux/delay.h>
#include <linux/pci.h>
#include <linux/time64.h>

#define __STDC_WANT_LIB_EXT1__ 1
#include <linux/string.h>
#include <linux/platform_data/i2c-ocores.h>

/***********************************************
 *       variable define
 * *********************************************/
#define DRVNAME                        "as9817_32_fpga"
#define OCORES_I2C_DRVNAME             "ocores-i2c"

#define PORT_NUM                       (32 + 2)  /* 32 OSFPs/QSFPDDs + 2 SFP28s */

#define I2C_BUS_CLK_400K

/*
 * PCIE BAR0 address
 */
#define BAR0_NUM                       0
#define BAR1_NUM                       1
#define BAR2_NUM                       2
#define REGION_LEN                     0xFF
#define FPGA_PCI_VENDOR_ID             0x10ee
#define FPGA_PCI_DEVICE_ID             0x7021

#define FPGA_PCIE_START_OFFSET         0x0000
#define FPGA_MAJOR_VER_REG             0x01
#define FPGA_MINOR_VER_REG             0x02
#define SPI_BUSY_MASK_CPLD1            0x01
#define SPI_BUSY_MASK_CPLD2            0x02

#define FPGA_PCIE_START_OFFSET         0x0000
#define FPGA_BOARD_INFO_REG            (FPGA_PCIE_START_OFFSET + 0x00)

/* CPLD 1 */
#define CPLD1_PCIE_START_OFFSET        0x2000
#define CPLD1_MAJOR_VER_REG            (CPLD1_PCIE_START_OFFSET + 0x00)
#define CPLD1_MINOR_VER_REG            (CPLD1_PCIE_START_OFFSET + 0x01)
#define XCVR_P7_P0_LPMODE_REG          (CPLD1_PCIE_START_OFFSET + 0x70)
#define XCVR_P15_P8_LPMODE_REG         (CPLD1_PCIE_START_OFFSET + 0x71)
#define XCVR_P7_P0_RESET_REG           (CPLD1_PCIE_START_OFFSET + 0x78)
#define XCVR_P15_P8_RESET_REG          (CPLD1_PCIE_START_OFFSET + 0x79)
#define XCVR_P7_P0_PRESENT_REG         (CPLD1_PCIE_START_OFFSET + 0x88)
#define XCVR_P15_P8_PRESENT_REG        (CPLD1_PCIE_START_OFFSET + 0x89)


/* CPLD 2 */
#define CPLD2_PCIE_START_OFFSET        0x3000
#define CPLD2_MAJOR_VER_REG            (CPLD2_PCIE_START_OFFSET + 0x00)
#define CPLD2_MINOR_VER_REG            (CPLD2_PCIE_START_OFFSET + 0x01)
#define SFP_TXFAULT_REG                (CPLD2_PCIE_START_OFFSET + 0x06)
#define SFP_TXDIS_REG                  (CPLD2_PCIE_START_OFFSET + 0x07)
#define SFP_RXLOSS_REG                 (CPLD2_PCIE_START_OFFSET + 0x08)
#define SFP_PRESENT_REG                (CPLD2_PCIE_START_OFFSET + 0x09)
#define XCVR_P23_P16_LPMODE_REG        (CPLD2_PCIE_START_OFFSET + 0x70)
#define XCVR_P31_P24_LPMODE_REG        (CPLD2_PCIE_START_OFFSET + 0x71)
#define XCVR_P23_P16_RESET_REG         (CPLD2_PCIE_START_OFFSET + 0x78)
#define XCVR_P31_P24_RESET_REG         (CPLD2_PCIE_START_OFFSET + 0x79)
#define XCVR_P23_P16_PRESENT_REG       (CPLD2_PCIE_START_OFFSET + 0x88)
#define XCVR_P31_P24_PRESENT_REG       (CPLD2_PCIE_START_OFFSET + 0x89)

/* DC-SCM CPLD */
#define CPLD3_PCIE_START_OFFSET        0x4000
#define CPLD3_BOARD_INFO_REG           (CPLD3_PCIE_START_OFFSET + 0x00)
#define CPLD3_VERSION_REG              (CPLD3_PCIE_START_OFFSET + 0x01)

/***********************************************
 *       macro define
 * *********************************************/
#define pcie_err(fmt, args...) \
        printk(KERN_ERR "["DRVNAME"]: " fmt " ", ##args)

#define pcie_info(fmt, args...) \
        printk(KERN_ERR "["DRVNAME"]: " fmt " ", ##args)


#define LOCK(lock)      \
do {                                                \
    spin_lock(lock);                                \
} while (0)

#define UNLOCK(lock)    \
do {                                                \
    spin_unlock(lock);                              \
} while (0)

#define TRANSCEIVER_ATTR_ID(index) \
    MODULE_PRESENT_##index =     (index - 1), \
    MODULE_RESET_##index =       (index - 1) + (PORT_NUM * 1), \
    MODULE_LPMODE_##index =      (index - 1) + (PORT_NUM * 2)

#define SFP_TRANSCEIVER_ATTR_ID(index) \
    MODULE_PRESENT_##index =     (index - 1), \
    MODULE_TX_DISABLE_##index =  (index - 1) + (PORT_NUM * 3), \
    MODULE_TX_FAULT_##index =    (index - 1) + (PORT_NUM * 4), \
    MODULE_RX_LOS_##index =      (index - 1) + (PORT_NUM * 5)

/*
 * MODULE_PRESENT_1     ... MODULE_PRESENT_34    =>    0 ...  33
 * MODULE_RESET_1       ... MODULE_RESET_32      =>   34 ...  65
 * MODULE_LPMODE_1      ... MODULE_LPMODE_32     =>   68 ...  99
 * MODULE_TX_DISABLE_33 ... MODULE_TX_DISABLE_34 =>  132 ... 133
 * MODULE_TX_FAULT_33   ... MODULE_TX_FAULT_34   =>  151 ... 152
 * MODULE_RX_LOS_33     ... MODULE_RX_LOS_34     =>  170 ... 171
 */

/***********************************************
 *       structure & variable declare
 * *********************************************/
typedef struct pci_fpga_device_s {
    void  __iomem *data_base_addr0;
    void  __iomem *data_base_addr1;
    void  __iomem *data_base_addr2;
    resource_size_t data_region1;
    resource_size_t data_region2;
    struct pci_dev  *pci_dev;
    struct platform_device *fpga_i2c[PORT_NUM];
} pci_fpga_device_t;

/*fpga port status*/
struct as9817_32_fpga_data {
    u8                  cpld_reg[3];
    unsigned long       last_updated;    /* In jiffies */
    pci_fpga_device_t   pci_fpga_dev;
};

static struct platform_device *pdev = NULL;
extern spinlock_t cpld_access_lock;
extern int wait_spi(u32 mask, unsigned long timeout);
extern void __iomem *spi_busy_reg;

/***********************************************
 *       enum define
 * *********************************************/
enum fpga_sysfs_attributes {
    /* transceiver attributes */
    TRANSCEIVER_ATTR_ID(1),
    TRANSCEIVER_ATTR_ID(2),
    TRANSCEIVER_ATTR_ID(3),
    TRANSCEIVER_ATTR_ID(4),
    TRANSCEIVER_ATTR_ID(5),
    TRANSCEIVER_ATTR_ID(6),
    TRANSCEIVER_ATTR_ID(7),
    TRANSCEIVER_ATTR_ID(8),
    TRANSCEIVER_ATTR_ID(9),
    TRANSCEIVER_ATTR_ID(10),
    TRANSCEIVER_ATTR_ID(11),
    TRANSCEIVER_ATTR_ID(12),
    TRANSCEIVER_ATTR_ID(13),
    TRANSCEIVER_ATTR_ID(14),
    TRANSCEIVER_ATTR_ID(15),
    TRANSCEIVER_ATTR_ID(16),
    TRANSCEIVER_ATTR_ID(17),
    TRANSCEIVER_ATTR_ID(18),
    TRANSCEIVER_ATTR_ID(19),
    TRANSCEIVER_ATTR_ID(20),
    TRANSCEIVER_ATTR_ID(21),
    TRANSCEIVER_ATTR_ID(22),
    TRANSCEIVER_ATTR_ID(23),
    TRANSCEIVER_ATTR_ID(24),
    TRANSCEIVER_ATTR_ID(25),
    TRANSCEIVER_ATTR_ID(26),
    TRANSCEIVER_ATTR_ID(27),
    TRANSCEIVER_ATTR_ID(28),
    TRANSCEIVER_ATTR_ID(29),
    TRANSCEIVER_ATTR_ID(30),
    TRANSCEIVER_ATTR_ID(31),
    TRANSCEIVER_ATTR_ID(32),
    SFP_TRANSCEIVER_ATTR_ID(33),
    SFP_TRANSCEIVER_ATTR_ID(34),
    CPLD1_VERSION,
    CPLD2_VERSION,
    CPLD3_VERSION,
    CPLD1_REG,
    CPLD2_REG,
    CPLD3_REG,
};


/***********************************************
 *       function declare
 * *********************************************/
static ssize_t reg_read(struct device *dev, struct device_attribute *da,
             char *buf);
static ssize_t reg_write(struct device *dev, struct device_attribute *da,
            const char *buf, size_t count);
static ssize_t status_read(struct device *dev, struct device_attribute *da,
             char *buf);
static ssize_t status_write(struct device *dev, struct device_attribute *da,
            const char *buf, size_t count);

#define DECLARE_TRANSCEIVER_SENSOR_DEVICE_ATTR(index) \
    static SENSOR_DEVICE_ATTR(module_present_##index, S_IRUGO, status_read, NULL, MODULE_PRESENT_##index); \
    static SENSOR_DEVICE_ATTR(module_reset_##index, S_IRUGO|S_IWUSR, status_read, status_write, MODULE_RESET_##index); \
    static SENSOR_DEVICE_ATTR(module_lp_mode_##index, S_IRUGO|S_IWUSR, status_read, status_write, MODULE_LPMODE_##index)
#define DECLARE_TRANSCEIVER_ATTR(index) \
    &sensor_dev_attr_module_present_##index.dev_attr.attr, \
    &sensor_dev_attr_module_reset_##index.dev_attr.attr, \
    &sensor_dev_attr_module_lp_mode_##index.dev_attr.attr

#define DECLARE_SFP_TRANSCEIVER_SENSOR_DEVICE_ATTR(index) \
    static SENSOR_DEVICE_ATTR(module_present_##index, S_IRUGO, status_read, NULL, MODULE_PRESENT_##index); \
    static SENSOR_DEVICE_ATTR(module_tx_disable_##index, S_IRUGO | S_IWUSR, status_read, status_write, MODULE_TX_DISABLE_##index); \
    static SENSOR_DEVICE_ATTR(module_tx_fault_##index, S_IRUGO, status_read, NULL, MODULE_TX_FAULT_##index); \
    static SENSOR_DEVICE_ATTR(module_rx_los_##index, S_IRUGO, status_read, NULL, MODULE_RX_LOS_##index)
#define DECLARE_SFP_TRANSCEIVER_ATTR(index)  \
    &sensor_dev_attr_module_present_##index.dev_attr.attr, \
    &sensor_dev_attr_module_tx_disable_##index.dev_attr.attr, \
    &sensor_dev_attr_module_rx_los_##index.dev_attr.attr, \
    &sensor_dev_attr_module_tx_fault_##index.dev_attr.attr


/* transceiver attributes */
DECLARE_TRANSCEIVER_SENSOR_DEVICE_ATTR(1);
DECLARE_TRANSCEIVER_SENSOR_DEVICE_ATTR(2);
DECLARE_TRANSCEIVER_SENSOR_DEVICE_ATTR(3);
DECLARE_TRANSCEIVER_SENSOR_DEVICE_ATTR(4);
DECLARE_TRANSCEIVER_SENSOR_DEVICE_ATTR(5);
DECLARE_TRANSCEIVER_SENSOR_DEVICE_ATTR(6);
DECLARE_TRANSCEIVER_SENSOR_DEVICE_ATTR(7);
DECLARE_TRANSCEIVER_SENSOR_DEVICE_ATTR(8);
DECLARE_TRANSCEIVER_SENSOR_DEVICE_ATTR(9);
DECLARE_TRANSCEIVER_SENSOR_DEVICE_ATTR(10);
DECLARE_TRANSCEIVER_SENSOR_DEVICE_ATTR(11);
DECLARE_TRANSCEIVER_SENSOR_DEVICE_ATTR(12);
DECLARE_TRANSCEIVER_SENSOR_DEVICE_ATTR(13);
DECLARE_TRANSCEIVER_SENSOR_DEVICE_ATTR(14);
DECLARE_TRANSCEIVER_SENSOR_DEVICE_ATTR(15);
DECLARE_TRANSCEIVER_SENSOR_DEVICE_ATTR(16);
DECLARE_TRANSCEIVER_SENSOR_DEVICE_ATTR(17);
DECLARE_TRANSCEIVER_SENSOR_DEVICE_ATTR(18);
DECLARE_TRANSCEIVER_SENSOR_DEVICE_ATTR(19);
DECLARE_TRANSCEIVER_SENSOR_DEVICE_ATTR(20);
DECLARE_TRANSCEIVER_SENSOR_DEVICE_ATTR(21);
DECLARE_TRANSCEIVER_SENSOR_DEVICE_ATTR(22);
DECLARE_TRANSCEIVER_SENSOR_DEVICE_ATTR(23);
DECLARE_TRANSCEIVER_SENSOR_DEVICE_ATTR(24);
DECLARE_TRANSCEIVER_SENSOR_DEVICE_ATTR(25);
DECLARE_TRANSCEIVER_SENSOR_DEVICE_ATTR(26);
DECLARE_TRANSCEIVER_SENSOR_DEVICE_ATTR(27);
DECLARE_TRANSCEIVER_SENSOR_DEVICE_ATTR(28);
DECLARE_TRANSCEIVER_SENSOR_DEVICE_ATTR(29);
DECLARE_TRANSCEIVER_SENSOR_DEVICE_ATTR(30);
DECLARE_TRANSCEIVER_SENSOR_DEVICE_ATTR(31);
DECLARE_TRANSCEIVER_SENSOR_DEVICE_ATTR(32);
DECLARE_SFP_TRANSCEIVER_SENSOR_DEVICE_ATTR(33);
DECLARE_SFP_TRANSCEIVER_SENSOR_DEVICE_ATTR(34);


static SENSOR_DEVICE_ATTR(cpld1_version, S_IRUGO, status_read, NULL, CPLD1_VERSION);
static SENSOR_DEVICE_ATTR(cpld2_version, S_IRUGO, status_read, NULL, CPLD2_VERSION);
static SENSOR_DEVICE_ATTR(cpld3_version, S_IRUGO, status_read, NULL, CPLD3_VERSION);
static SENSOR_DEVICE_ATTR(cpld1_reg, S_IRUGO|S_IWUSR, reg_read, reg_write, CPLD1_REG);
static SENSOR_DEVICE_ATTR(cpld2_reg, S_IRUGO|S_IWUSR, reg_read, reg_write, CPLD2_REG);
static SENSOR_DEVICE_ATTR(cpld3_reg, S_IRUGO|S_IWUSR, reg_read, reg_write, CPLD3_REG);



static struct attribute *fpga_transceiver_attributes[] = {
    DECLARE_TRANSCEIVER_ATTR(1),
    DECLARE_TRANSCEIVER_ATTR(2),
    DECLARE_TRANSCEIVER_ATTR(3),
    DECLARE_TRANSCEIVER_ATTR(4),
    DECLARE_TRANSCEIVER_ATTR(5),
    DECLARE_TRANSCEIVER_ATTR(6),
    DECLARE_TRANSCEIVER_ATTR(7),
    DECLARE_TRANSCEIVER_ATTR(8),
    DECLARE_TRANSCEIVER_ATTR(9),
    DECLARE_TRANSCEIVER_ATTR(10),
    DECLARE_TRANSCEIVER_ATTR(11),
    DECLARE_TRANSCEIVER_ATTR(12),
    DECLARE_TRANSCEIVER_ATTR(13),
    DECLARE_TRANSCEIVER_ATTR(14),
    DECLARE_TRANSCEIVER_ATTR(15),
    DECLARE_TRANSCEIVER_ATTR(16),
    DECLARE_TRANSCEIVER_ATTR(17),
    DECLARE_TRANSCEIVER_ATTR(18),
    DECLARE_TRANSCEIVER_ATTR(19),
    DECLARE_TRANSCEIVER_ATTR(20),
    DECLARE_TRANSCEIVER_ATTR(21),
    DECLARE_TRANSCEIVER_ATTR(22),
    DECLARE_TRANSCEIVER_ATTR(23),
    DECLARE_TRANSCEIVER_ATTR(24),
    DECLARE_TRANSCEIVER_ATTR(25),
    DECLARE_TRANSCEIVER_ATTR(26),
    DECLARE_TRANSCEIVER_ATTR(27),
    DECLARE_TRANSCEIVER_ATTR(28),
    DECLARE_TRANSCEIVER_ATTR(29),
    DECLARE_TRANSCEIVER_ATTR(30),
    DECLARE_TRANSCEIVER_ATTR(31),
    DECLARE_TRANSCEIVER_ATTR(32),
    DECLARE_SFP_TRANSCEIVER_ATTR(33),
    DECLARE_SFP_TRANSCEIVER_ATTR(34),
    &sensor_dev_attr_cpld1_version.dev_attr.attr,
    &sensor_dev_attr_cpld2_version.dev_attr.attr,
    &sensor_dev_attr_cpld3_version.dev_attr.attr,
    &sensor_dev_attr_cpld1_reg.dev_attr.attr,
    &sensor_dev_attr_cpld2_reg.dev_attr.attr,
    &sensor_dev_attr_cpld3_reg.dev_attr.attr,
    NULL
};

static const struct attribute_group fpga_port_stat_group = {
    .attrs = fpga_transceiver_attributes,
};

struct attribute_mapping {
    u16 attr_base;
    u16 reg;
    u8 revert;
};

// Define an array of attribute mappings
static struct attribute_mapping attribute_mappings[] = {
    [MODULE_PRESENT_1 ... MODULE_PRESENT_8] = {MODULE_PRESENT_1, XCVR_P7_P0_PRESENT_REG, 1},
    [MODULE_PRESENT_9 ... MODULE_PRESENT_16] = {MODULE_PRESENT_9, XCVR_P15_P8_PRESENT_REG, 1},
    [MODULE_PRESENT_17 ... MODULE_PRESENT_24] = {MODULE_PRESENT_17, XCVR_P23_P16_PRESENT_REG, 1},
    [MODULE_PRESENT_25 ... MODULE_PRESENT_32] = {MODULE_PRESENT_25, XCVR_P31_P24_PRESENT_REG, 1},
    [MODULE_PRESENT_33 ... MODULE_PRESENT_34] = {MODULE_PRESENT_33, SFP_PRESENT_REG, 1},

    [MODULE_LPMODE_1 ... MODULE_LPMODE_8] = {MODULE_LPMODE_1, XCVR_P7_P0_LPMODE_REG, 1},
    [MODULE_LPMODE_9 ... MODULE_LPMODE_16] = {MODULE_LPMODE_9, XCVR_P15_P8_LPMODE_REG, 1},
    [MODULE_LPMODE_17 ... MODULE_LPMODE_24] = {MODULE_LPMODE_17, XCVR_P23_P16_LPMODE_REG, 1},
    [MODULE_LPMODE_25 ... MODULE_LPMODE_32] = {MODULE_LPMODE_25, XCVR_P31_P24_LPMODE_REG, 1},

    [MODULE_RESET_1 ... MODULE_RESET_8] = {MODULE_RESET_1, XCVR_P7_P0_RESET_REG, 1},
    [MODULE_RESET_9 ... MODULE_RESET_16] = {MODULE_RESET_9, XCVR_P15_P8_RESET_REG, 1},
    [MODULE_RESET_17 ... MODULE_RESET_24] = {MODULE_RESET_17, XCVR_P23_P16_RESET_REG, 1},
    [MODULE_RESET_25 ... MODULE_RESET_32] = {MODULE_RESET_25, XCVR_P31_P24_RESET_REG, 1},

    [MODULE_TX_DISABLE_33 ... MODULE_TX_DISABLE_34] ={MODULE_TX_DISABLE_33, SFP_TXDIS_REG, 0},
    [MODULE_TX_FAULT_33 ... MODULE_TX_FAULT_34] = {MODULE_TX_FAULT_33, SFP_TXFAULT_REG, 0},
    [MODULE_RX_LOS_33 ... MODULE_RX_LOS_34] = {MODULE_RX_LOS_33, SFP_RXLOSS_REG, 0},
};

static inline unsigned int fpga_read(const void __iomem *addr, u32 spi_mask)
{
    wait_spi(spi_mask, usecs_to_jiffies(20));
    return ioread8(addr);
}

static inline void fpga_write(void __iomem *addr, u8 val, u32 spi_mask)
{
    wait_spi(spi_mask, usecs_to_jiffies(20));
    iowrite8(val, addr);
}

static ssize_t reg_read(struct device *dev, struct device_attribute *da, char *buf)
{
    struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
    struct as9817_32_fpga_data *fpga_ctl = dev_get_drvdata(dev);
    void __iomem *addr;
    ssize_t ret = -EINVAL;
    u8 reg_val;
    u32 spi_mask;

    switch(attr->index)
    {
        case CPLD1_REG:
            addr = fpga_ctl->pci_fpga_dev.data_base_addr1 + 
                   CPLD1_PCIE_START_OFFSET + 
                   fpga_ctl->cpld_reg[attr->index - CPLD1_REG];
            spi_mask = SPI_BUSY_MASK_CPLD1;
            break;
        case CPLD2_REG:
            addr = fpga_ctl->pci_fpga_dev.data_base_addr2 + 
                   CPLD2_PCIE_START_OFFSET + 
                   fpga_ctl->cpld_reg[attr->index - CPLD1_REG];
            spi_mask = SPI_BUSY_MASK_CPLD2;
            break;
        case CPLD3_REG:
            addr = fpga_ctl->pci_fpga_dev.data_base_addr2 + 
                   CPLD3_PCIE_START_OFFSET + 
                   fpga_ctl->cpld_reg[attr->index - CPLD1_REG];
            spi_mask = SPI_BUSY_MASK_CPLD2;
            break;
        default:
            ret = -EINVAL;
            goto exit;
    }

    LOCK(&cpld_access_lock);
    reg_val = fpga_read(addr, spi_mask);
    UNLOCK(&cpld_access_lock);
    ret = sprintf(buf, "0x%02x\n", reg_val);

exit:
    return ret;
}

static ssize_t reg_write(struct device *dev, struct device_attribute *da,
                         const char *buf, size_t count)
{
    struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
    struct as9817_32_fpga_data *fpga_ctl = dev_get_drvdata(dev);
    void __iomem *base;
    int args;
    char *opt, tmp[32] = {0};
    char *tmp_p;
    size_t copy_size;
    u8 input[2] = {0};
    u32 spi_mask;

    switch(attr->index)
    {
        case CPLD1_REG:
            base = fpga_ctl->pci_fpga_dev.data_base_addr1 + 
                   CPLD1_PCIE_START_OFFSET;
            spi_mask = SPI_BUSY_MASK_CPLD1;
            break;
        case CPLD2_REG:
            base = fpga_ctl->pci_fpga_dev.data_base_addr2 + 
                   CPLD2_PCIE_START_OFFSET;
            spi_mask = SPI_BUSY_MASK_CPLD2;
            break;
        case CPLD3_REG:
            base = fpga_ctl->pci_fpga_dev.data_base_addr2 + 
                   CPLD3_PCIE_START_OFFSET;
            spi_mask = SPI_BUSY_MASK_CPLD2;
            break;
        default:
            return -EINVAL;
    }

    copy_size = (count < sizeof(tmp)) ? count : sizeof(tmp) - 1;
    #ifdef __STDC_LIB_EXT1__
    memcpy_s(tmp, copy_size, buf, copy_size);
    #else
    memcpy(tmp, buf, copy_size);
    #endif
    tmp[copy_size] = '\0';

    args = 0;
    tmp_p = tmp;
    while (args < 2 && (opt = strsep(&tmp_p, " ")) != NULL) {
        if (kstrtou8(opt, 16, &input[args]) == 0) {
            args++;
        }
    }

    switch(args)
    {
        case 2:
            /* Write value to register */
            LOCK(&cpld_access_lock);
            fpga_write(base + input[0], input[1], spi_mask);
            UNLOCK(&cpld_access_lock);
            break;
        case 1:
            /* Read value from register */
            fpga_ctl->cpld_reg[attr->index - CPLD1_REG] = input[0];
            break;
        default:
            return -EINVAL;
    }

    return count;
}

static ssize_t status_read(struct device *dev, struct device_attribute *da, char *buf)
{
    struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
    struct as9817_32_fpga_data *fpga_ctl = dev_get_drvdata(dev);
    ssize_t ret = -EINVAL;
    u16 reg;
    u8 major, minor, reg_val;
    u8 bits_shift;

    switch(attr->index)
    {
        case CPLD1_VERSION:
            LOCK(&cpld_access_lock);
            reg = CPLD1_MAJOR_VER_REG;
            major = fpga_read(fpga_ctl->pci_fpga_dev.data_base_addr1 + reg, 
                              SPI_BUSY_MASK_CPLD1);
            reg = CPLD1_MINOR_VER_REG;
            minor = fpga_read(fpga_ctl->pci_fpga_dev.data_base_addr1 + reg, 
                              SPI_BUSY_MASK_CPLD1);
            UNLOCK(&cpld_access_lock);

            ret = sprintf(buf, "%d.%d\n", major, minor);
            break;
        case CPLD2_VERSION:
            LOCK(&cpld_access_lock);
            reg = CPLD2_MAJOR_VER_REG;
            major = fpga_read(fpga_ctl->pci_fpga_dev.data_base_addr2 + reg, 
                              SPI_BUSY_MASK_CPLD2);
            reg = CPLD2_MINOR_VER_REG;
            minor = fpga_read(fpga_ctl->pci_fpga_dev.data_base_addr2 + reg, 
                              SPI_BUSY_MASK_CPLD2);
            UNLOCK(&cpld_access_lock);

            ret = sprintf(buf, "%d.%d\n", major, minor);
            break;
        case CPLD3_VERSION:
            LOCK(&cpld_access_lock);
            reg = CPLD3_VERSION_REG;
            major = fpga_read(fpga_ctl->pci_fpga_dev.data_base_addr2 + reg, 
                              SPI_BUSY_MASK_CPLD2);
            UNLOCK(&cpld_access_lock);

            ret = sprintf(buf, "%d\n", major);
            break;
        case MODULE_PRESENT_1 ... MODULE_RX_LOS_34:
            reg = attribute_mappings[attr->index].reg;
            LOCK(&cpld_access_lock);
            if ((reg & 0xF000) == CPLD1_PCIE_START_OFFSET) {
                reg_val = fpga_read(fpga_ctl->pci_fpga_dev.data_base_addr1 + reg, 
                                    SPI_BUSY_MASK_CPLD1);
            } else if ((reg & 0xF000) == CPLD2_PCIE_START_OFFSET) {
                reg_val = fpga_read(fpga_ctl->pci_fpga_dev.data_base_addr2 + reg, 
                                    SPI_BUSY_MASK_CPLD2);
            }
            UNLOCK(&cpld_access_lock);

            bits_shift = attr->index - attribute_mappings[attr->index].attr_base;
            reg_val = (reg_val >> bits_shift) & 0x01;
            if (attribute_mappings[attr->index].revert) {
                reg_val = !reg_val;
            }

            ret = sprintf(buf, "%u\n", reg_val);
            break;
        default:
            break;
    }

    return ret;
}

static ssize_t status_write(struct device *dev, struct device_attribute *da,
                            const char *buf, size_t count)
{
    struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
    struct as9817_32_fpga_data *fpga_ctl = dev_get_drvdata(dev);
    void __iomem *addr;
    int status;
    u16 reg;
    u8 input;
    u8 reg_val, bit_mask, should_set_bit;
    u32 spi_mask;

    status = kstrtou8(buf, 10, &input);
    if (status) {
        return status;
    }

    reg = attribute_mappings[attr->index].reg;
    if ((reg & 0xF000) == CPLD1_PCIE_START_OFFSET) {
        spi_mask = SPI_BUSY_MASK_CPLD1;
        addr = fpga_ctl->pci_fpga_dev.data_base_addr1;
    } else if ((reg & 0xF000) == CPLD2_PCIE_START_OFFSET) {
        spi_mask = SPI_BUSY_MASK_CPLD2;
        addr = fpga_ctl->pci_fpga_dev.data_base_addr2;
    }
    bit_mask = 0x01 << (attr->index - attribute_mappings[attr->index].attr_base);
    should_set_bit = attribute_mappings[attr->index].revert ? !input : input;

    LOCK(&cpld_access_lock);
    reg_val = fpga_read(addr + reg, spi_mask);
    if (should_set_bit) {
        reg_val |= bit_mask;
    } else {
        reg_val &= ~bit_mask;
    }
    fpga_write(addr + reg, reg_val, spi_mask);
    UNLOCK(&cpld_access_lock);

    return count;
}

struct _port_data {
    u16 offset;
    u16 mask; /* SPI Busy mask : 0x01 --> CPLD1, 0x02 --> CPLD2 */
};
/* ============PCIe Bar Offset to I2C Master Mapping============== */
static const struct _port_data port[PORT_NUM]= {
    {0x2100, SPI_BUSY_MASK_CPLD1},/* 0x2100 - 0x2110  CPLD1 I2C Master Port1 */
    {0x2120, SPI_BUSY_MASK_CPLD1},/* 0x2120 - 0x2130  CPLD1 I2C Master Port2 */
    {0x2140, SPI_BUSY_MASK_CPLD1},/* 0x2140 - 0x2150  CPLD1 I2C Master Port3 */
    {0x2160, SPI_BUSY_MASK_CPLD1},/* 0x2160 - 0x2170  CPLD1 I2C Master Port4 */
    {0x2180, SPI_BUSY_MASK_CPLD1},/* 0x2180 - 0x2190  CPLD1 I2C Master Port5 */
    {0x21A0, SPI_BUSY_MASK_CPLD1},/* 0x21A0 - 0x21B0  CPLD1 I2C Master Port6 */
    {0x21C0, SPI_BUSY_MASK_CPLD1},/* 0x21C0 - 0x21D0  CPLD1 I2C Master Port7 */
    {0x21E0, SPI_BUSY_MASK_CPLD1},/* 0x21E0 - 0x21F0  CPLD1 I2C Master Port8 */
    {0x2200, SPI_BUSY_MASK_CPLD1},/* 0x2200 - 0x2210  CPLD1 I2C Master Port9 */
    {0x2220, SPI_BUSY_MASK_CPLD1},/* 0x2220 - 0x2230  CPLD1 I2C Master Port10 */
    {0x2240, SPI_BUSY_MASK_CPLD1},/* 0x2240 - 0x2250  CPLD1 I2C Master Port11 */
    {0x2260, SPI_BUSY_MASK_CPLD1},/* 0x2260 - 0x2270  CPLD1 I2C Master Port12 */
    {0x2280, SPI_BUSY_MASK_CPLD1},/* 0x2280 - 0x2290  CPLD1 I2C Master Port13 */
    {0x22A0, SPI_BUSY_MASK_CPLD1},/* 0x22A0 - 0x22B0  CPLD1 I2C Master Port14 */
    {0x22C0, SPI_BUSY_MASK_CPLD1},/* 0x22C0 - 0x22D0  CPLD1 I2C Master Port15 */
    {0x22E0, SPI_BUSY_MASK_CPLD1},/* 0x22E0 - 0x22F0  CPLD1 I2C Master Port16 */
    {0x3100, SPI_BUSY_MASK_CPLD2},/* 0x3100 - 0x3110  CPLD2 I2C Master Port17 */
    {0x3120, SPI_BUSY_MASK_CPLD2},/* 0x3120 - 0x3130  CPLD2 I2C Master Port18 */
    {0x3140, SPI_BUSY_MASK_CPLD2},/* 0x3140 - 0x3150  CPLD2 I2C Master Port19 */
    {0x3160, SPI_BUSY_MASK_CPLD2},/* 0x3160 - 0x3170  CPLD2 I2C Master Port20 */
    {0x3180, SPI_BUSY_MASK_CPLD2},/* 0x3180 - 0x3190  CPLD2 I2C Master Port21 */
    {0x31A0, SPI_BUSY_MASK_CPLD2},/* 0x31A0 - 0x31B0  CPLD2 I2C Master Port22 */
    {0x31C0, SPI_BUSY_MASK_CPLD2},/* 0x31C0 - 0x31D0  CPLD2 I2C Master Port23 */
    {0x31E0, SPI_BUSY_MASK_CPLD2},/* 0x31E0 - 0x31F0  CPLD2 I2C Master Port24 */
    {0x3200, SPI_BUSY_MASK_CPLD2},/* 0x3200 - 0x3210  CPLD2 I2C Master Port25 */
    {0x3220, SPI_BUSY_MASK_CPLD2},/* 0x3220 - 0x3230  CPLD2 I2C Master Port26 */
    {0x3240, SPI_BUSY_MASK_CPLD2},/* 0x3240 - 0x3250  CPLD2 I2C Master Port27 */
    {0x3260, SPI_BUSY_MASK_CPLD2},/* 0x3260 - 0x3270  CPLD2 I2C Master Port28 */
    {0x3280, SPI_BUSY_MASK_CPLD2},/* 0x3280 - 0x3290  CPLD2 I2C Master Port29 */
    {0x32A0, SPI_BUSY_MASK_CPLD2},/* 0x32A0 - 0x32B0  CPLD2 I2C Master Port30 */
    {0x32C0, SPI_BUSY_MASK_CPLD2},/* 0x32C0 - 0x34D0  CPLD2 I2C Master Port31 */
    {0x32E0, SPI_BUSY_MASK_CPLD2},/* 0x32E0 - 0x3490  CPLD2 I2C Master Port32 */
    {0x3500, SPI_BUSY_MASK_CPLD2},/* 0x3500 - 0x3510  CPLD2 I2C Master SFP-28 Port33 */
    {0x3520, SPI_BUSY_MASK_CPLD2},/* 0x3520 - 0x3530  CPLD2 I2C Master SFP-28 Port34 */
};

static struct ocores_i2c_platform_data as9817_32_platform_data = {
    .reg_io_width = 1,
    .reg_shift = 2,
    /*
     * PRER_L and PRER_H are calculated based on clock_khz and bus_khz 
     * in i2c-ocores.c:ocores_init.
     */
#ifdef I2C_BUS_CLK_400K
    /* SCL 400KHZ in FPGA spec. => PRER_L = 0x0B, PRER_H = 0x00 */
    .clock_khz = 24000,
    .bus_khz = 400,
#elif defined(I2C_BUS_CLK_100K)
    /* SCL 100KHZ in FPGA spec. => PRER_L = 0x2F, PRER_H = 0x00 */
    .clock_khz = 24000,
    .bus_khz = 100,
#endif
};

struct platform_device *ocore_i2c_device_add(unsigned int id, unsigned long bar_base,
                                             unsigned int offset)
{
    struct resource res = DEFINE_RES_MEM(bar_base + offset, 0x20);
    struct platform_device *pdev;
    int err;

    pdev = platform_device_alloc(OCORES_I2C_DRVNAME, id);
    if (!pdev) {
        err = -ENOMEM;
        pcie_err("Port%u device allocation failed (%d)\n", (id & 0xFF), err);
        goto exit;
    }

    err = platform_device_add_resources(pdev, &res, 1);
    if (err) {
        pcie_err("Port%u device resource addition failed (%d)\n", (id & 0xFF), err);
        goto exit_device_put;
    }

    err = platform_device_add_data(pdev, &as9817_32_platform_data,
                       sizeof(struct ocores_i2c_platform_data));
    if (err) {
        pcie_err("Port%u platform data allocation failed (%d)\n", (id & 0xFF), err);
        goto exit_device_put;
    }

    err = platform_device_add(pdev);
    if (err) {
        pcie_err("Port%u device addition failed (%d)\n", (id & 0xFF), err);
        goto exit_device_put;
    }

    return pdev;

exit_device_put:
    platform_device_put(pdev);
exit:
    return NULL;
}

static int as9817_32_pcie_fpga_stat_probe(struct platform_device *pdev)
{
    struct device *dev = &pdev->dev;
    struct as9817_32_fpga_data *fpga_ctl;
    struct pci_dev *pcidev;
    struct resource *ret;
    int i;
    int status = 0, err = 0;
    unsigned long bar_base;
    unsigned int val;

    fpga_ctl = devm_kzalloc(dev, sizeof(struct as9817_32_fpga_data), GFP_KERNEL);
    if (!fpga_ctl) {
        return -ENOMEM;
    }
    platform_set_drvdata(pdev, fpga_ctl);

    pcidev = pci_get_device(FPGA_PCI_VENDOR_ID, FPGA_PCI_DEVICE_ID, NULL);
     if (!pcidev) {
        dev_err(dev, "Cannot found PCI device(%x:%x)\n",
                     FPGA_PCI_VENDOR_ID, FPGA_PCI_DEVICE_ID);
        return -ENODEV;
    }
    fpga_ctl->pci_fpga_dev.pci_dev = pcidev;

    err = pci_enable_device(pcidev);
    if (err != 0) {
        dev_err(dev, "Cannot enable PCI device(%x:%x)\n",
                     FPGA_PCI_VENDOR_ID, FPGA_PCI_DEVICE_ID);
        status = -ENODEV;
        goto exit_pci_disable;
    }
    /* enable PCI bus-mastering */
    pci_set_master(pcidev);

    /*
     * Detect platform for changing the setting behavior of LP mode.
     */
    fpga_ctl->pci_fpga_dev.data_base_addr0 = pci_iomap(pcidev, BAR0_NUM, 0);
    if (fpga_ctl->pci_fpga_dev.data_base_addr0 == NULL) {
        dev_err(dev, "Failed to map BAR0\n");
        status = -EIO;
        goto exit_pci_disable;
    }
    dev_info(dev, "(BAR%d resource: Mapped to virtual address = 0x%p, Length=0x%x)", BAR0_NUM,
                  fpga_ctl->pci_fpga_dev.data_base_addr0, REGION_LEN);

    val = ioread8(fpga_ctl->pci_fpga_dev.data_base_addr0 + FPGA_BOARD_INFO_REG);
    switch (val & 0x0C)
    {
        case 0x00: /* OSFP */
        case 0x08: /* OSFP DC-SCM */
            dev_info(dev, "Platform: AS9817-32O(0x%02x)\n", val);
            break;
        case 0x04: /* QDD */
        case 0x0C: /* QDD DC-SCM */
            for (i = MODULE_LPMODE_1; i <= MODULE_LPMODE_32; i++) {
                attribute_mappings[i].revert = 0;
            }
            dev_info(dev, "Platform: AS9817-32D(0x%02x)\n", val);
            break;
        default:
            dev_warn(dev, "Unknown platform detected\n");
            break;
    }

    /*
     * The register address of SPI Busy is 0x33.
     * It can not only read one byte. It needs to read four bytes from 0x30.
     * The value is obtained by '(ioread32(spi_busy_reg) >> 24) & 0xFF'.
     */
    spi_busy_reg = fpga_ctl->pci_fpga_dev.data_base_addr0 + 0x30;



    /*
     * Cannot use 'pci_request_regions(pcidev, DRVNAME)' 
     * to request all Region 1, Region 2 because another 
     * address will be allocated by the i2c-ocores.ko.
     */
    fpga_ctl->pci_fpga_dev.data_base_addr1 = pci_iomap(pcidev, BAR1_NUM, 0);
    if (fpga_ctl->pci_fpga_dev.data_base_addr1 == NULL) {
        dev_err(dev, "Failed to map BAR1\n");
        status = -EIO;
        goto exit_pci_iounmap0;
    }
    fpga_ctl->pci_fpga_dev.data_region1 = pci_resource_start(pcidev, BAR1_NUM) + CPLD1_PCIE_START_OFFSET;
    ret = request_mem_region(fpga_ctl->pci_fpga_dev.data_region1, REGION_LEN, DRVNAME"_cpld1");
    if (ret == NULL) {
        dev_err(dev, "[%s] cannot request region\n", DRVNAME"_cpld1");
        status = -EIO;
        goto exit_pci_iounmap1;
    }
    dev_info(dev, "(BAR%d resource: Mapped to virtual address = 0x%p, Length=0x%x)", BAR1_NUM,
                  fpga_ctl->pci_fpga_dev.data_base_addr1, REGION_LEN);

    fpga_ctl->pci_fpga_dev.data_base_addr2 = pci_iomap(pcidev, BAR2_NUM, 0);
    if (fpga_ctl->pci_fpga_dev.data_base_addr2 == NULL) {
        dev_err(dev, "Failed to map BAR2\n");
        status = -EIO;
        goto exit_pci_release1;
    }
    fpga_ctl->pci_fpga_dev.data_region2 = pci_resource_start(pcidev, BAR2_NUM) + CPLD2_PCIE_START_OFFSET;
    ret = request_mem_region(fpga_ctl->pci_fpga_dev.data_region2, REGION_LEN, DRVNAME"_cpld1");
    if (ret == NULL) {
        dev_err(dev, "[%s] cannot request region\n", DRVNAME"_cpld1");
        status = -EIO;
        goto exit_pci_iounmap2;
    }
    dev_info(dev, "(BAR%d resource: Mapped to virtual address = 0x%p, Length=0x%x)", BAR2_NUM,
                  fpga_ctl->pci_fpga_dev.data_base_addr2, REGION_LEN);

    /* Create I2C ocore devices first, then create the FPGA sysfs.
     * To prevent the application from accessing an ocore device 
     * that has not been fully created due to the port status 
     * being present.
     */

    /*
     * Create ocore_i2c device for OSFP EEPROM
     */
    for (i = 0; i < PORT_NUM; i++) {
        switch (i)
        {
            case 0 ... 15:
                bar_base = pci_resource_start(pcidev, BAR1_NUM);
                break;
            case 16 ... 31:
                bar_base = pci_resource_start(pcidev, BAR2_NUM);
                break;
            default:
                break;
        }
        fpga_ctl->pci_fpga_dev.fpga_i2c[i] = 
            ocore_i2c_device_add((i | (port[i].mask << 8)), bar_base, port[i].offset);
        if (IS_ERR(fpga_ctl->pci_fpga_dev.fpga_i2c[i])) {
            status = PTR_ERR(fpga_ctl->pci_fpga_dev.fpga_i2c[i]);
            dev_err(dev, "rc:%d, unload Port%u[0x%ux] device\n", 
                         status, i, port[i].offset);
            goto unregister_devices;
        }
    }

    status = sysfs_create_group(&pdev->dev.kobj, &fpga_port_stat_group);
    if (status) {
        goto unregister_devices;
    }

    return 0;

unregister_devices:
    while (i > 0) {
        i--;
        platform_device_unregister(fpga_ctl->pci_fpga_dev.fpga_i2c[i]);
    }
    release_mem_region(fpga_ctl->pci_fpga_dev.data_region2, REGION_LEN);
exit_pci_iounmap2:
    pci_iounmap(fpga_ctl->pci_fpga_dev.pci_dev, fpga_ctl->pci_fpga_dev.data_base_addr2);
exit_pci_release1:
    release_mem_region(fpga_ctl->pci_fpga_dev.data_region1, REGION_LEN);
exit_pci_iounmap1:
    pci_iounmap(fpga_ctl->pci_fpga_dev.pci_dev, fpga_ctl->pci_fpga_dev.data_base_addr1);
exit_pci_iounmap0:
    spi_busy_reg = NULL;
    pci_iounmap(fpga_ctl->pci_fpga_dev.pci_dev, fpga_ctl->pci_fpga_dev.data_base_addr0);
exit_pci_disable:
    pci_disable_device(fpga_ctl->pci_fpga_dev.pci_dev);


    return status;
}

static int as9817_32_pcie_fpga_stat_remove(struct platform_device *pdev)
{
    struct as9817_32_fpga_data *fpga_ctl = platform_get_drvdata(pdev);

    if (pci_is_enabled(fpga_ctl->pci_fpga_dev.pci_dev)) {
        int i;

        sysfs_remove_group(&pdev->dev.kobj, &fpga_port_stat_group);
        /* Unregister ocore_i2c device */
        for (i = 0; i < PORT_NUM; i++) {
            platform_device_unregister(fpga_ctl->pci_fpga_dev.fpga_i2c[i]);
        }
        spi_busy_reg = NULL;
        pci_iounmap(fpga_ctl->pci_fpga_dev.pci_dev, fpga_ctl->pci_fpga_dev.data_base_addr0);
        pci_iounmap(fpga_ctl->pci_fpga_dev.pci_dev, fpga_ctl->pci_fpga_dev.data_base_addr1);
        pci_iounmap(fpga_ctl->pci_fpga_dev.pci_dev, fpga_ctl->pci_fpga_dev.data_base_addr2);
        release_mem_region(fpga_ctl->pci_fpga_dev.data_region1, REGION_LEN);
        release_mem_region(fpga_ctl->pci_fpga_dev.data_region2, REGION_LEN);
        pci_disable_device(fpga_ctl->pci_fpga_dev.pci_dev);
    }

    return 0;
}

static struct platform_driver pcie_fpga_port_stat_driver = {
    .probe      = as9817_32_pcie_fpga_stat_probe,
    .remove     = as9817_32_pcie_fpga_stat_remove,
    .driver     = {
        .owner = THIS_MODULE,
        .name  = DRVNAME,
    },
};

static int __init as9817_32_pcie_fpga_init(void)
{
    int status = 0;

    /*
     * Create FPGA platform driver and device
     */
    status = platform_driver_register(&pcie_fpga_port_stat_driver);
    if (status < 0) {
        return status;
    }

    pdev = platform_device_register_simple(DRVNAME, -1, NULL, 0);
    if (IS_ERR(pdev)) {
        status = PTR_ERR(pdev);
        goto exit_pci;
    }

    return status;

exit_pci:
    platform_driver_unregister(&pcie_fpga_port_stat_driver);

    return status;
}

static void __exit as9817_32_pcie_fpga_exit(void)
{
    platform_device_unregister(pdev);
    platform_driver_unregister(&pcie_fpga_port_stat_driver);
}


module_init(as9817_32_pcie_fpga_init);
module_exit(as9817_32_pcie_fpga_exit);

MODULE_AUTHOR("Roger Ho <roger530_ho@accton.com>");
MODULE_DESCRIPTION("AS9817-32O/AS9817-32D FPGA via PCIE");
MODULE_LICENSE("GPL");
