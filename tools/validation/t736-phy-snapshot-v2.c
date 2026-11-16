// SPDX-License-Identifier: GPL-2.0
/* One-shot RTL8211F v2 standard-page comparison. No configuration/advertisement/reset writes.
 * One MDIO bus lock covers save/select/read/restore/readback. Its only PHY
 * write is the page selector; restore is attempted and checked after errors.
 * Reads consume BMSR latch-low state and STAT1000 master/slave fault latch
 * (bit15) and idle-error counter (bits7:0), per RTL8211F GBSR register10.
 * This module neither restarts negotiation nor changes link settings.
 */
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/phy.h>
#include <linux/rtnetlink.h>
#include <net/net_namespace.h>

struct sample { u16 page, reg; const char *name; };
static const struct sample samples[] = {
 {0xa42, MII_CTRL1000, "PAGE_A42_CTRL1000"},
 {0xa42, MII_STAT1000, "PAGE_A42_STAT1000"},
 {0, MII_BMCR, "BMCR"}, {0, MII_BMSR, "BMSR_first"},
 {0, MII_BMSR, "BMSR_current"}, {0, MII_ADVERTISE, "ADVERTISE"},
 {0, MII_LPA, "LPA"}, {0, MII_CTRL1000, "CTRL1000"},
 {0, MII_STAT1000, "STAT1000"}, {0, MII_ESTATUS, "ESTATUS"},
 {0xa43, 0x18, "PHYCR1"}, {0xa43, 0x19, "PHYCR2"},
 {0xd08, 0x11, "TX_DELAY"}, {0xd08, 0x15, "RX_DELAY"},
};

static int __init snapshot_init(void)
{
 struct net_device *netdev;
 struct phy_device *phy;
 int oldpage, after = -1, value, restore, ret = 0, selected_page;
 int values[ARRAY_SIZE(samples)];
 unsigned int i, j;

 rtnl_lock();
 netdev = dev_get_by_name(&init_net, "end0");
 if (!netdev) { ret = -ENODEV; goto unlock; }
 phy = netdev->phydev;
 if (!phy || phy->phy_id != 0x001cc916 || phy->mdio.addr != 1 ||
     !phy->drv || !phy->drv->read_page || !phy->drv->write_page) {
  ret = -ENODEV; goto release;
 }
 phy_lock_mdio_bus(phy);
 oldpage = __phy_read(phy, 0x1f);
 if (oldpage < 0) { ret = oldpage; phy_unlock_mdio_bus(phy); goto release; }
 selected_page = oldpage;
 for (i = 0; i < ARRAY_SIZE(samples); i++) {
  if (selected_page != samples[i].page) {
   ret = __phy_write(phy, 0x1f, samples[i].page);
   if (ret < 0) break;
   selected_page = samples[i].page;
  }
  value = __phy_read(phy, samples[i].reg);
  if (value < 0) {
   ret = value; break;
  }
  values[i] = value;
 }
 /* Attempt restoration even if selecting a page failed partway through. */
 restore = __phy_write(phy, 0x1f, oldpage);
 after = __phy_read(phy, 0x1f);
 if (restore < 0 || after < 0 || after != oldpage) {
  ret = restore < 0 ? restore : after < 0 ? after : -EIO;
 }
 phy_unlock_mdio_bus(phy);
 pr_info("t736-phy-snapshot: begin id=%08x address=%d original_page=%04x\n",
         phy->phy_id, phy->mdio.addr, oldpage);
 pr_info("t736-phy-snapshot: reads consume BMSR latches and STAT1000 fault/idle-error status\n");
 for (j = 0; j < i; j++)
  pr_info("t736-phy-snapshot: %s page=%04x reg=%02x value=%04x\n",
          samples[j].name, samples[j].page, samples[j].reg, values[j]);
 pr_info("t736-phy-snapshot: end original_page=%04x final_page=%04x restore_rc=%d rc=%d; writes_limited_to_page_selector=1\n",
         oldpage, after, restore, ret);
release:
 dev_put(netdev);
unlock:
 rtnl_unlock();
 return ret;
}
static void __exit snapshot_exit(void) { }
module_init(snapshot_init);
module_exit(snapshot_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("T736 RTL8211F page0xa42/page0 comparison with locked reads");
