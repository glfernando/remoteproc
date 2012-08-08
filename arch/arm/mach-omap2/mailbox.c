/*
 * Mailbox reservation modules for OMAP2/3
 *
 * Copyright (C) 2006-2009 Nokia Corporation
 * Written by: Hiroshi DOYU <Hiroshi.DOYU@nokia.com>
 *        and  Paul Mundt
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 */

#include <linux/module.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/pm_runtime.h>
#include <plat/mailbox.h>
#include <mach/irqs.h>

#define MAILBOX_REVISION		0x000
#define MAILBOX_MESSAGE(m)		(0x040 + 4 * (m))
#define MAILBOX_FIFOSTATUS(m)		(0x080 + 4 * (m))
#define MAILBOX_MSGSTATUS(m)		(0x0c0 + 4 * (m))
#define MAILBOX_IRQSTATUS(u)		(0x100 + 8 * (u))
#define MAILBOX_IRQENABLE(u)		(0x104 + 8 * (u))

#define OMAP4_MAILBOX_IRQSTATUS(u)	(0x104 + 0x10 * (u))
#define OMAP4_MAILBOX_IRQENABLE(u)	(0x108 + 0x10 * (u))
#define OMAP4_MAILBOX_IRQENABLE_CLR(u)	(0x10c + 0x10 * (u))

#define MAILBOX_IRQ_NEWMSG(m)		(1 << (2 * (m)))
#define MAILBOX_IRQ_NOTFULL(m)		(1 << (2 * (m) + 1))

/*
 * As per TRM, omap mailboxes are limited to 4 messages per mailbox
 * in the hardware queue.
 */
#define MAX_MSG_HW_QUEUE		4

static void __iomem *mbox_base;

struct omap_mbox2_fifo {
	unsigned long msg;
	unsigned long fifo_stat;
	unsigned long msg_stat;
};

struct mbox_context {
	unsigned long tx_msg[MAX_MSG_HW_QUEUE];
	unsigned long tx_msg_stat;
	unsigned long rx_msg[MAX_MSG_HW_QUEUE];
	unsigned long rx_msg_stat;
	unsigned long irqenable_bit;
};

struct omap_mbox2_priv {
	struct omap_mbox2_fifo tx_fifo;
	struct omap_mbox2_fifo rx_fifo;
	unsigned long irqenable;
	unsigned long irqstatus;
	u32 newmsg_bit;
	u32 notfull_bit;
	unsigned long irqdisable;
	struct mbox_context ctx;
};

static void omap2_mbox_enable_irq(struct omap_mbox *mbox,
				  omap_mbox_type_t irq);

static inline unsigned int mbox_read_reg(size_t ofs)
{
	return __raw_readl(mbox_base + ofs);
}

static inline void mbox_write_reg(u32 val, size_t ofs)
{
	__raw_writel(val, mbox_base + ofs);
}

/* Mailbox H/W preparations */
static int omap2_mbox_startup(struct omap_mbox *mbox)
{
	u32 l;

	pm_runtime_enable(mbox->dev->parent);
	pm_runtime_get_sync(mbox->dev->parent);

	l = mbox_read_reg(MAILBOX_REVISION);
	pr_debug("omap mailbox rev %d.%d\n", (l & 0xf0) >> 4, (l & 0x0f));

	return 0;
}

static void omap2_mbox_shutdown(struct omap_mbox *mbox)
{
	pm_runtime_put_sync(mbox->dev->parent);
	pm_runtime_disable(mbox->dev->parent);
}

/* Mailbox FIFO handle functions */
static mbox_msg_t omap2_mbox_fifo_read(struct omap_mbox *mbox)
{
	struct omap_mbox2_fifo *fifo =
		&((struct omap_mbox2_priv *)mbox->priv)->rx_fifo;
	return (mbox_msg_t) mbox_read_reg(fifo->msg);
}

static void omap2_mbox_fifo_write(struct omap_mbox *mbox, mbox_msg_t msg)
{
	struct omap_mbox2_fifo *fifo =
		&((struct omap_mbox2_priv *)mbox->priv)->tx_fifo;
	mbox_write_reg(msg, fifo->msg);
}

static int omap2_mbox_fifo_empty(struct omap_mbox *mbox)
{
	struct omap_mbox2_fifo *fifo =
		&((struct omap_mbox2_priv *)mbox->priv)->rx_fifo;
	return (mbox_read_reg(fifo->msg_stat) == 0);
}

static int omap2_mbox_fifo_full(struct omap_mbox *mbox)
{
	struct omap_mbox2_fifo *fifo =
		&((struct omap_mbox2_priv *)mbox->priv)->tx_fifo;
	return mbox_read_reg(fifo->fifo_stat);
}

/* Mailbox IRQ handle functions */
static void _omap2_mbox_enable_irq(u32 bit, unsigned long irqenable)
{
	u32 l = mbox_read_reg(irqenable);
	l |= bit;
	mbox_write_reg(l, irqenable);
}

static void omap2_mbox_enable_irq(struct omap_mbox *mbox,
		omap_mbox_type_t irq)
{
	struct omap_mbox2_priv *p = mbox->priv;
	u32 bit = (irq == IRQ_TX) ? p->notfull_bit : p->newmsg_bit;

	_omap2_mbox_enable_irq(bit, p->irqenable);

	/* For context purposes, we don't care saving irqdisable registers */
	p->ctx.irqenable_bit |= bit;
}

static void _omap2_mbox_disable_irq(u32 bit, unsigned long irqdisable)
{
	if (!cpu_is_omap44xx())
		bit = mbox_read_reg(irqdisable) & ~bit;

	mbox_write_reg(bit, irqdisable);
}

static void omap2_mbox_disable_irq(struct omap_mbox *mbox,
		omap_mbox_type_t irq)
{
	struct omap_mbox2_priv *p = mbox->priv;
	u32 bit = (irq == IRQ_TX) ? p->notfull_bit : p->newmsg_bit;

	_omap2_mbox_disable_irq(bit, p->irqdisable);

	/* For context purposes, we don't care saving irqdisable registers */
	p->ctx.irqenable_bit &= ~bit;
}

static void omap2_mbox_ack_irq(struct omap_mbox *mbox,
		omap_mbox_type_t irq)
{
	struct omap_mbox2_priv *p = mbox->priv;
	u32 bit = (irq == IRQ_TX) ? p->notfull_bit : p->newmsg_bit;

	mbox_write_reg(bit, p->irqstatus);

	/* Flush posted write for irq status to avoid spurious interrupts */
	mbox_read_reg(p->irqstatus);
}

static int omap2_mbox_is_irq(struct omap_mbox *mbox,
		omap_mbox_type_t irq)
{
	struct omap_mbox2_priv *p = mbox->priv;
	u32 bit = (irq == IRQ_TX) ? p->notfull_bit : p->newmsg_bit;
	u32 enable = mbox_read_reg(p->irqenable);
	u32 status = mbox_read_reg(p->irqstatus);

	return (int)(enable & status & bit);
}

static void omap2_mbox_save_ctx(struct omap_mbox *mbox)
{
	int i;
	struct omap_mbox2_priv *p = mbox->priv;

	/*
	 * Disable not-full irq, otherwise it will be triggered as soon
	 * as the pending messages are saved.
	 */
	if (p->ctx.irqenable_bit & p->notfull_bit)
		_omap2_mbox_disable_irq(p->notfull_bit, p->irqdisable);

	/* Save pending messages in TX fifo */
	p->ctx.tx_msg_stat = mbox_read_reg(p->tx_fifo.msg_stat);
	for (i = 0; i < p->ctx.tx_msg_stat; i++)
		p->ctx.tx_msg[i] = mbox_read_reg(p->tx_fifo.msg);

	if (unlikely(mbox_read_reg(p->tx_fifo.msg_stat) != 0))
		pr_warn("spurious tx messages not saved\n");

	/* Save pending messages in RX fifo */
	p->ctx.rx_msg_stat = mbox_read_reg(p->rx_fifo.msg_stat);
	for (i = 0; i < p->ctx.rx_msg_stat; i++)
		p->ctx.rx_msg[i] = mbox_read_reg(p->rx_fifo.msg);

	if (unlikely(mbox_read_reg(p->rx_fifo.msg_stat) != 0))
		pr_warn("spurious rx messages not saved\n");
}

static void omap2_mbox_restore_ctx(struct omap_mbox *mbox)
{
	int i;
	struct omap_mbox2_priv *p = mbox->priv;

	/* Restore pending messages in TX fifo */
	if (unlikely(mbox_read_reg(p->tx_fifo.msg_stat)))
		pr_warn("Unexpected messages in TX queue\n");

	for (i = 0; i < p->ctx.tx_msg_stat; i++)
		mbox_write_reg(p->ctx.tx_msg[i], p->tx_fifo.msg);

	/* Save pending messages in RX fifo */
	if (unlikely(mbox_read_reg(p->rx_fifo.msg_stat)))
		pr_warn("Unexpected messages in RX queue\n");

	for (i = 0; i < p->ctx.rx_msg_stat; i++)
		mbox_write_reg(p->ctx.rx_msg[i], p->rx_fifo.msg);

	/* Enable interrupt bits */
	if (p->ctx.irqenable_bit & p->notfull_bit)
		_omap2_mbox_enable_irq(p->notfull_bit, p->irqenable);

	if (p->ctx.irqenable_bit & p->newmsg_bit)
		_omap2_mbox_enable_irq(p->newmsg_bit, p->irqenable);
}

static struct omap_mbox_ops omap2_mbox_ops = {
	.type		= OMAP_MBOX_TYPE2,
	.startup	= omap2_mbox_startup,
	.shutdown	= omap2_mbox_shutdown,
	.fifo_read	= omap2_mbox_fifo_read,
	.fifo_write	= omap2_mbox_fifo_write,
	.fifo_empty	= omap2_mbox_fifo_empty,
	.fifo_full	= omap2_mbox_fifo_full,
	.enable_irq	= omap2_mbox_enable_irq,
	.disable_irq	= omap2_mbox_disable_irq,
	.ack_irq	= omap2_mbox_ack_irq,
	.is_irq		= omap2_mbox_is_irq,
	.save_ctx	= omap2_mbox_save_ctx,
	.restore_ctx	= omap2_mbox_restore_ctx,
};

/*
 * MAILBOX 0: ARM -> DSP,
 * MAILBOX 1: ARM <- DSP.
 * MAILBOX 2: ARM -> IVA,
 * MAILBOX 3: ARM <- IVA.
 */

/* FIXME: the following structs should be filled automatically by the user id */

#if defined(CONFIG_ARCH_OMAP3) || defined(CONFIG_ARCH_OMAP2)
/* DSP */
static struct omap_mbox2_priv omap2_mbox_dsp_priv = {
	.tx_fifo = {
		.msg		= MAILBOX_MESSAGE(0),
		.fifo_stat	= MAILBOX_FIFOSTATUS(0),
		.msg_stat	= MAILBOX_MSGSTATUS(0),
	},
	.rx_fifo = {
		.msg		= MAILBOX_MESSAGE(1),
		.msg_stat	= MAILBOX_MSGSTATUS(1),
	},
	.irqenable	= MAILBOX_IRQENABLE(0),
	.irqstatus	= MAILBOX_IRQSTATUS(0),
	.notfull_bit	= MAILBOX_IRQ_NOTFULL(0),
	.newmsg_bit	= MAILBOX_IRQ_NEWMSG(1),
	.irqdisable	= MAILBOX_IRQENABLE(0),
};

struct omap_mbox mbox_dsp_info = {
	.name	= "dsp",
	.ops	= &omap2_mbox_ops,
	.priv	= &omap2_mbox_dsp_priv,
};
#endif

#if defined(CONFIG_ARCH_OMAP3)
struct omap_mbox *omap3_mboxes[] = { &mbox_dsp_info, NULL };
#endif

#if defined(CONFIG_SOC_OMAP2420)
/* IVA */
static struct omap_mbox2_priv omap2_mbox_iva_priv = {
	.tx_fifo = {
		.msg		= MAILBOX_MESSAGE(2),
		.fifo_stat	= MAILBOX_FIFOSTATUS(2),
		.msg_stat	= MAILBOX_MSGSTATUS(2),
	},
	.rx_fifo = {
		.msg		= MAILBOX_MESSAGE(3),
		.msg_stat	= MAILBOX_MSGSTATUS(3),
	},
	.irqenable	= MAILBOX_IRQENABLE(3),
	.irqstatus	= MAILBOX_IRQSTATUS(3),
	.notfull_bit	= MAILBOX_IRQ_NOTFULL(2),
	.newmsg_bit	= MAILBOX_IRQ_NEWMSG(3),
	.irqdisable	= MAILBOX_IRQENABLE(3),
};

static struct omap_mbox mbox_iva_info = {
	.name	= "iva",
	.ops	= &omap2_mbox_ops,
	.priv	= &omap2_mbox_iva_priv,
};
#endif

#ifdef CONFIG_ARCH_OMAP2
struct omap_mbox *omap2_mboxes[] = {
	&mbox_dsp_info,
#ifdef CONFIG_SOC_OMAP2420
	&mbox_iva_info,
#endif
	NULL
};
#endif

#if defined(CONFIG_ARCH_OMAP4)
/* OMAP4 */
static struct omap_mbox2_priv omap2_mbox_1_priv = {
	.tx_fifo = {
		.msg		= MAILBOX_MESSAGE(0),
		.fifo_stat	= MAILBOX_FIFOSTATUS(0),
		.msg_stat	= MAILBOX_MSGSTATUS(0),
	},
	.rx_fifo = {
		.msg		= MAILBOX_MESSAGE(1),
		.msg_stat	= MAILBOX_MSGSTATUS(1),
	},
	.irqenable	= OMAP4_MAILBOX_IRQENABLE(0),
	.irqstatus	= OMAP4_MAILBOX_IRQSTATUS(0),
	.notfull_bit	= MAILBOX_IRQ_NOTFULL(0),
	.newmsg_bit	= MAILBOX_IRQ_NEWMSG(1),
	.irqdisable	= OMAP4_MAILBOX_IRQENABLE_CLR(0),
};

struct omap_mbox mbox_1_info = {
	.name	= "mailbox-1",
	.ops	= &omap2_mbox_ops,
	.priv	= &omap2_mbox_1_priv,
};

static struct omap_mbox2_priv omap2_mbox_2_priv = {
	.tx_fifo = {
		.msg		= MAILBOX_MESSAGE(3),
		.fifo_stat	= MAILBOX_FIFOSTATUS(3),
		.msg_stat	= MAILBOX_MSGSTATUS(3),
	},
	.rx_fifo = {
		.msg		= MAILBOX_MESSAGE(2),
		.msg_stat	= MAILBOX_MSGSTATUS(2),
	},
	.irqenable	= OMAP4_MAILBOX_IRQENABLE(0),
	.irqstatus	= OMAP4_MAILBOX_IRQSTATUS(0),
	.notfull_bit	= MAILBOX_IRQ_NOTFULL(3),
	.newmsg_bit	= MAILBOX_IRQ_NEWMSG(2),
	.irqdisable     = OMAP4_MAILBOX_IRQENABLE_CLR(0),
};

struct omap_mbox mbox_2_info = {
	.name	= "mailbox-2",
	.ops	= &omap2_mbox_ops,
	.priv	= &omap2_mbox_2_priv,
};

struct omap_mbox *omap4_mboxes[] = { &mbox_1_info, &mbox_2_info, NULL };
#endif

static int __devinit omap2_mbox_probe(struct platform_device *pdev)
{
	struct resource *mem;
	int ret;
	struct omap_mbox **list;

	if (false)
		;
#if defined(CONFIG_ARCH_OMAP3)
	else if (cpu_is_omap34xx()) {
		list = omap3_mboxes;

		list[0]->irq = platform_get_irq(pdev, 0);
	}
#endif
#if defined(CONFIG_ARCH_OMAP2)
	else if (cpu_is_omap2430()) {
		list = omap2_mboxes;

		list[0]->irq = platform_get_irq(pdev, 0);
	} else if (cpu_is_omap2420()) {
		list = omap2_mboxes;

		list[0]->irq = platform_get_irq_byname(pdev, "dsp");
		list[1]->irq = platform_get_irq_byname(pdev, "iva");
	}
#endif
#if defined(CONFIG_ARCH_OMAP4)
	else if (cpu_is_omap44xx()) {
		list = omap4_mboxes;

		list[0]->irq = list[1]->irq = platform_get_irq(pdev, 0);
	}
#endif
	else {
		pr_err("%s: platform not supported\n", __func__);
		return -ENODEV;
	}

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	mbox_base = ioremap(mem->start, resource_size(mem));
	if (!mbox_base)
		return -ENOMEM;

	ret = omap_mbox_register(&pdev->dev, list);
	if (ret) {
		iounmap(mbox_base);
		return ret;
	}

	return 0;
}

static int __devexit omap2_mbox_remove(struct platform_device *pdev)
{
	omap_mbox_unregister();
	iounmap(mbox_base);
	return 0;
}

static int _mailbox_runtime_suspend(struct device *dev, void *data)
{
	struct omap_mbox *mbox = dev_get_drvdata(dev);
	omap2_mbox_save_ctx(mbox);
	return 0;
}

static int omap_mailbox_runtime_suspend(struct device *dev)
{
	return device_for_each_child(dev, NULL, _mailbox_runtime_suspend);
}

static int _mailbox_runtime_resume(struct device *dev, void *data)
{
	struct omap_mbox *mbox = dev_get_drvdata(dev);
	omap2_mbox_restore_ctx(mbox);
	return 0;
}

static int omap_mailbox_runtime_resume(struct device *dev)
{
	return device_for_each_child(dev, NULL, _mailbox_runtime_resume);
}

static const struct dev_pm_ops mailbox_pm_ops = {
	SET_RUNTIME_PM_OPS(omap_mailbox_runtime_suspend,
			   omap_mailbox_runtime_resume,
			   NULL)
};

static struct platform_driver omap2_mbox_driver = {
	.probe = omap2_mbox_probe,
	.remove = __devexit_p(omap2_mbox_remove),
	.driver = {
		.name = "omap-mailbox",
		.pm = &mailbox_pm_ops,
	},
};

static int __init omap2_mbox_init(void)
{
	return platform_driver_register(&omap2_mbox_driver);
}

static void __exit omap2_mbox_exit(void)
{
	platform_driver_unregister(&omap2_mbox_driver);
}

module_init(omap2_mbox_init);
module_exit(omap2_mbox_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("omap mailbox: omap2/3/4 architecture specific functions");
MODULE_AUTHOR("Hiroshi DOYU <Hiroshi.DOYU@nokia.com>");
MODULE_AUTHOR("Paul Mundt");
MODULE_ALIAS("platform:omap2-mailbox");
