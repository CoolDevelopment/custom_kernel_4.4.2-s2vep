/*
* Haptic driver for broadcom primary vibrator(class D) 
*
* Copyright (C) 2012 kc45.kim@samsung.com
*
* This program is free software. you can redistribute it and/or modify it
* under the terms of the GNU Public License version 2 as
* published by the Free Software Foundation
*
*/

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/workqueue.h>
#include <linux/types.h>
#include <linux/platform_device.h>
#include <linux/switch.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/serio.h>
#include <linux/gpio.h>
#include <linux/proc_fs.h>
#include <linux/delay.h>
#include <linux/regulator/consumer.h>
#include <linux/mfd/bcmpmu.h>
#include <linux/wakelock.h>
#include <linux/pwm/pwm.h>
#include <linux/isa1000_vibrator.h>

//#include <alsa/asoundlib.h>

#include "../staging/android/timed_output.h"

typedef struct
{
	struct pwm_device 	*pwm;
	struct timed_output_dev timed_dev;
	struct timer_list vib_timer;
	struct work_struct off_work;
	struct regulator *vib_regulator;
	const char *vib_vcc;
	int (*gpio_en) (bool) ;
	u16 pwm_duty;
	u16 pwm_period;	
	u16 pwm_polarity;	
	int	pwm_started;
	int	initialized;
}t_vib_desc;


static t_vib_desc vib_desc;
static int controlset(const char *name, unsigned int *value, int index);
static void vibrator_control(t_vib_desc *vib_iter, unsigned char onoff);
#ifndef CONFIG_VIBETONZ
static int pwm_val = 50;

#define GPIO_MOTOR_EN  189
#endif

void vibtonz_en(bool en)
{
	t_vib_desc *vib_iter =&vib_desc;

	printk("%s %s \n", __func__, (en?"enabled":"disabled"));
	if( vib_iter->initialized == 0) return;
	if(en)
	{
		vib_iter->gpio_en(en);
		pwm_start(vib_iter->pwm);
	}
	else
	{
		pwm_stop(vib_iter->pwm);
		vib_iter->gpio_en(en);
	}
}

EXPORT_SYMBOL(vibtonz_en);

void vibtonz_pwm(int nForce)
{
	static int prev_duty=0;
	t_vib_desc *vib_iter =&vib_desc;
	int pwm_period=0, pwm_duty = 0;

	printk("%s : %d \n", __func__, nForce);
	if( vib_iter->initialized == 0) return;

	pwm_period = vib_iter->pwm_period;
	pwm_duty = pwm_period/2 + ((pwm_period/2 - 2) *nForce) /127;

	if(pwm_duty > vib_iter->pwm_duty)
	{
		pwm_duty = vib_iter->pwm_duty;
	}
	else if(pwm_period - pwm_duty > vib_iter->pwm_duty)
	{
		pwm_duty = pwm_period - vib_iter->pwm_duty;
	}
	
	pwm_set_period_ns(vib_iter->pwm,vib_iter->pwm_period); 	
	pwm_set_polarity(vib_iter->pwm, vib_iter->pwm_polarity);
	pwm_set_duty_ns(vib_iter->pwm, pwm_duty); 
}

EXPORT_SYMBOL(vibtonz_pwm);

#ifndef CONFIG_VIBETONZ
void drv2603_gpio_en(bool en)
{

	if (en == 1)
		gpio_set_value(GPIO_MOTOR_EN, 1);
	else
		gpio_set_value(GPIO_MOTOR_EN, 0);
		
	return;
	
}
#endif

static void vibrator_control(t_vib_desc *vib_iter, unsigned char onoff)
{

#if 0
	printk("%s : Vibrator %s\n", __func__, (onoff)?"on":"off");

	if( onoff == 1)
	{
		if(!regulator_is_enabled(vib_iter->vib_regulator))
		{
			regulator_enable(vib_iter->vib_regulator);
		}
	}
	else if( onoff == 0)
	{
		if(regulator_is_enabled(vib_iter->vib_regulator))
		{
			regulator_disable(vib_iter->vib_regulator);
		}
	}
#endif
#ifndef CONFIG_VIBETONZ
	if (onoff == 1)
	{
		drv2603_gpio_en(1);
		pwm_start(vib_iter->pwm);
		vibtonz_pwm(pwm_val);
	}
	else
	{
		pwm_stop(vib_iter->pwm);
		drv2603_gpio_en(0);
	}
#endif

	return;
}

static void vibrator_enable_set_timeout(struct timed_output_dev *sdev, int timeout)
{
	t_vib_desc *vib_iter=container_of(sdev, t_vib_desc, timed_dev);
	int valid_timeout;

	if(timeout == 0)
	{
		vibrator_control(vib_iter, 0);
		return;
	}

	vibrator_control(vib_iter, 1);

	printk(KERN_INFO "%s : Vibrator timeout = %d \n", __func__, timeout);

	mod_timer(&vib_iter->vib_timer, jiffies + msecs_to_jiffies(timeout));
}

static void vibrator_off_work_func(struct work_struct *work)
{
	t_vib_desc *vib_iter=container_of(work, t_vib_desc, off_work);

	vibrator_control(vib_iter, 0);
}

static void on_vibrate_timer_expired(unsigned long x)
{
	t_vib_desc *vib_iter = (t_vib_desc *)x;

	schedule_work(&vib_iter->off_work);
}

static void vibrator_get_remaining_time(struct timed_output_dev *sdev)
{
	t_vib_desc *vib_iter=container_of(sdev, t_vib_desc, timed_dev);
	int retTime=jiffies_to_msecs(jiffies-vib_iter->vib_timer.expires);
	printk(KERN_INFO "Vibrator : remaining time : %dms \n", retTime);
}

#ifndef CONFIG_VIBETONZ
static ssize_t pwm_val_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int count;

	count = sprintf(buf, "%lu\n", pwm_val);
	pr_debug("[VIB] pwm_val: %lu\n", pwm_val);

	return count;
}

ssize_t pwm_val_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t size)
{
	if (kstrtoul(buf, 0, &pwm_val))
		pr_err("[VIB] %s: error on storing pwm_val\n", __func__); 

	pr_info("[VIB] %s: pwm_val=%lu\n", __func__, pwm_val);

	return size;
}
static DEVICE_ATTR(pwm_val, S_IRUGO | S_IWUSR,
		pwm_val_show, pwm_val_store);

static int create_vibrator_sysfs(void)
{
	int ret;
	struct kobject *vibrator_kobj;
	vibrator_kobj = kobject_create_and_add("vibrator", NULL);
	if (unlikely(!vibrator_kobj))
		return -ENOMEM;

	ret = sysfs_create_file(vibrator_kobj,
			&dev_attr_pwm_val.attr);
	if (unlikely(ret < 0)) {
		pr_err("[VIB] sysfs_create_file failed: %d\n", ret);
		return ret;
	}

	return 0;
}
#endif


static int ss_brcm_haptic_probe(struct platform_device *pdev)
{
	struct platform_isa1000_vibrator_data *pdata = pdev->dev.platform_data;
	t_vib_desc *vib_iter;
	int ret=0;

	printk("ss_brcm_haptic_probe \n"); 

	/* vib_iter=kzalloc(sizeof(t_vib_desc), GFP_KERNEL);
	   if(vib_iter == NULL)
	   {
	   pr_err("%s : memory allocation failure \n", __func__);
	   return -ENOMEM;
	   } */
	vib_iter=&vib_desc;
#if 0
	vib_iter->vib_vcc = (const char *)pdata->regulator_id;
	printk(KERN_INFO "%s: Vibrator vcc=%s \n", __func__, vib_iter->vib_vcc);

	vib_iter->vib_regulator=regulator_get(NULL, vib_iter->vib_vcc);
	if(IS_ERR(vib_iter->vib_regulator))
	{
		printk(KERN_INFO "%s: failed to get regulator \n", __func__);
		return -ENODEV;
	}

	regulator_enable(vib_iter->vib_regulator);
#endif
#ifndef CONFIG_VIBETONZ
	vib_iter->gpio_en = &drv2603_gpio_en;
#else
	vib_iter->gpio_en = pdata->gpio_en;
#endif
	vib_iter->pwm = pwm_request(pdata->pwm_name, "vibrator");
	if (IS_ERR(vib_iter->pwm)) 
	{
		pr_err("[VIB] Failed to request pwm.\n");
		 return -EFAULT;
	}
	
	vib_iter->pwm_duty = pdata->pwm_duty;
	vib_iter->pwm_period = pdata->pwm_period_ns;
	vib_iter->pwm_polarity = pdata->polarity;

	pwm_set_polarity(vib_iter->pwm , vib_iter->pwm_polarity); 

	vib_iter->timed_dev.name="vibrator";
	vib_iter->timed_dev.enable=vibrator_enable_set_timeout;
	vib_iter->timed_dev.get_time=vibrator_get_remaining_time;

	ret = timed_output_dev_register(&vib_iter->timed_dev);
	if(ret < 0)
	{
		printk(KERN_ERR "Vibrator: timed_output dev registration failure\n");
		timed_output_dev_unregister(&vib_iter->timed_dev);
	}

	init_timer(&vib_iter->vib_timer);
	vib_iter->vib_timer.function = on_vibrate_timer_expired;
	vib_iter->vib_timer.data = (unsigned long)vib_iter;

	platform_set_drvdata(pdev, vib_iter);

	INIT_WORK(&vib_iter->off_work, vibrator_off_work_func);

#ifndef CONFIG_VIBETONZ
	create_vibrator_sysfs();
#endif
	
	vib_iter->initialized = 1;
	printk("%s : ss vibrator probe\n", __func__);
	return 0;

}

static int __devexit ss_brcm_haptic_remove(struct platform_device *pdev)
{
	t_vib_desc *vib_iter = platform_get_drvdata(pdev);
	timed_output_dev_unregister(&vib_iter->timed_dev);
	//regulator_put(vib_iter->vib_regulator);
	return 0;
}

static struct platform_driver ss_brcm_haptic_driver = {
	.probe = ss_brcm_haptic_probe,
	.remove = ss_brcm_haptic_remove,
	.driver = {
		.name = "drv2603-vibrator",
		.owner = THIS_MODULE,
	},
};

static void __init ss_brcm_haptic_init(void)
{
	printk("ss_haptic init \n");
	platform_driver_register(&ss_brcm_haptic_driver);
}

static void __exit ss_brcm_haptic_exit(void)
{
	platform_driver_unregister(&ss_brcm_haptic_driver);
}

late_initcall(ss_brcm_haptic_init);
module_exit(ss_brcm_haptic_exit);

MODULE_DESCRIPTION("Samsung Vibrator driver");
MODULE_LICENSE("GPL");

