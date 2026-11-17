/* Reading this attribute uses the existing R-TWI owner. It reports the PMIC
 * programmed setpoint, not an ADC measurement. There is no probe-time read.
 */
static ssize_t dcdc2_snapshot_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct t736_gpu_supply *p;
	u8 id = 0, enable = 0, dcdc2 = 0, phase = 0, feedback = 0;
	unsigned int selector, uv;
	int ret = -ENODEV;

	(void)attr;
	mutex_lock(&cpu_supply_owner_lock);
	p = cpu_supply_owner;
	if (!p || p->dev != dev)
		goto out_owner;
	mutex_lock(&p->lock);
	ret = pmic_read(p, 0x0e, &id);
	if (!ret && id != 3)
		ret = -ENODEV;
	if (!ret)
		ret = pmic_read(p, 0x10, &enable);
	if (!ret)
		ret = pmic_read(p, 0x13, &dcdc2);
	if (!ret)
		ret = pmic_read(p, 0x1b, &phase);
	if (!ret)
		ret = pmic_read(p, 0x1d, &feedback);
	mutex_unlock(&p->lock);
out_owner:
	mutex_unlock(&cpu_supply_owner_lock);
	if (ret)
		return ret;
	selector = dcdc2 & 0x7f;
	/* A joined DCDC2/3 rail cannot be interpreted as independent DCDC2. */
	if (!(enable & BIT(1)) || selector > 0x57 || (phase & BIT(6))) {
		dev_warn_ratelimited(dev,
			"DCDC2 snapshot invalid: enable=%02x dcdc2=%02x phase=%02x feedback=%02x\n",
			enable, dcdc2, phase, feedback);
		return -ERANGE;
	}
	uv = selector <= 0x46 ? 500000 + selector * 10000 :
		1220000 + (selector - 0x47) * 20000;
	return sysfs_emit(buf,
		"id=%02x enable=%02x dcdc2=%02x phase=%02x feedback=%02x dvm=%u remote_feedback=%u setpoint_uv=%u kind=programmed-setpoint\n",
		id, enable, dcdc2, phase, feedback, !!(dcdc2 & BIT(7)),
		!(feedback & BIT(0)), uv);
}
static DEVICE_ATTR_RO(dcdc2_snapshot);

static struct attribute *t736_pmic_attrs[] = {
	&dev_attr_dcdc2_snapshot.attr,
	NULL,
};
ATTRIBUTE_GROUPS(t736_pmic);
