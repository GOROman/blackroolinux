/* BLACKROO: Minimal irq setup stub for PlayStation.
 * The real IRQ handling is in int-handler.S and the original
 * irq.c which was overwritten. This provides setup_ps_irq. */
#include <linux/interrupt.h>
#include <linux/irq.h>

int setup_ps_irq(int irq, struct irqaction *action)
{
	return request_irq(irq, action->handler, action->flags,
	                   action->name, action->dev_id);
}
