// SPDX-License-Identifier: GPL-2.0-only
/* T736 DE_SYS and VO1 PCK600 only. Delay values: Tina pck600_domains.c.
 * Power policy sequence: upstream sun55i-pck600.c / ARM PPU specification.
 * Other PCK domains remain entirely outside this driver's register resources.
 */
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>

struct display_pd { struct generic_pm_domain genpd; void __iomem *base; };
struct display_power {
	struct display_pd pd[2];
	struct generic_pm_domain *domains[2];
	struct genpd_onecell_data data;
};
static int display_power_on(struct generic_pm_domain *domain)
{
	struct display_pd *pd=container_of(domain,struct display_pd,genpd);
	u32 v=readl(pd->base);
	writel((v & ~0xf) | 8,pd->base);
	readl(pd->base);
	return readl_poll_timeout(pd->base+8,v,(v & 0xf)==8,10,10000);
}
static int display_power_probe(struct platform_device *pdev)
{
	static const char * const names[]={ "t736-de-sys", "t736-vo1" };
	struct display_power *p;
	struct clk *clk;
	int i,ret;
	p=devm_kzalloc(&pdev->dev,sizeof(*p),GFP_KERNEL);
	if (!p) return -ENOMEM;
	clk=devm_clk_get_enabled(&pdev->dev,NULL);
	if (IS_ERR(clk)) return PTR_ERR(clk);
	for (i=0;i<2;i++) {
		struct display_pd *pd=&p->pd[i];
		pd->base=devm_platform_ioremap_resource(pdev,i);
		if (IS_ERR(pd->base)) { ret=PTR_ERR(pd->base); goto remove; }
		writel(0x1f1f1f,pd->base+0x170);
		writel(0x1f1f,pd->base+0x174);
		writel(0x08080808,pd->base+0xc00);
		writel(0x808,pd->base+0xc04);
		writel(8,pd->base+0xc10);
		pd->genpd.name=names[i]; pd->genpd.power_on=display_power_on;
		pd->genpd.flags=GENPD_FLAG_ALWAYS_ON;
		ret=display_power_on(&pd->genpd);
		if (ret) { dev_err(&pdev->dev,"%s power-on timeout\n",names[i]); goto remove; }
		ret=pm_genpd_init(&pd->genpd,NULL,false);
		if (ret) goto remove;
		p->domains[i]=&pd->genpd;
	}
	p->data.domains=p->domains; p->data.num_domains=2;
	ret=of_genpd_add_provider_onecell(pdev->dev.of_node,&p->data);
	if (!ret) return 0;
remove:
	while (i--) pm_genpd_remove(&p->pd[i].genpd);
	return ret;
}
static const struct of_device_id display_power_match[]={
	{ .compatible="allwinner,t736-display-power" }, { }
};
static struct platform_driver display_power_driver={
	.probe=display_power_probe,
	.driver={ .name="t736-display-power", .of_match_table=display_power_match,
		.suppress_bind_attrs=true },
};
builtin_platform_driver(display_power_driver);
