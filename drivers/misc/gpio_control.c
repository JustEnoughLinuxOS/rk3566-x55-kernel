#include <dt-bindings/gpio/gpio.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/fb.h>
#include <linux/backlight.h>
#include <linux/err.h>
#include <linux/pwm.h>
#include <linux/pwm_backlight.h>
#include <linux/slab.h>
#include <linux/proc_fs.h>
#include <linux/termios.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>

//#define INVALID_GPIO -1

struct proc_dir_entry *gpio_extend_dir;
static int major;
static struct class *rk_gpio_class = NULL;

static int gpios[10];
static int nr_gpios = 0;	

static  struct pwm_device *zed_pwm = NULL;
static unsigned int             pwm_period ;
static unsigned long            defalut_period ;										  
static struct of_device_id gpio_of_match[] = {
	{ .compatible = "gpio_ctrl" },
	{ }
};


MODULE_DEVICE_TABLE(of, gpio_of_match);




static ssize_t gpio_write_proc_string(struct file *file, const char __user *buffer,
					size_t count, loff_t *pos)
{
	printk("##%s##\n",__func__);
	return count;
}

static int gpio_str_proc_show(struct seq_file *m, void *v)
{
	//seq_printf(m, "%s\n",disp_str);
//	int ret;
	printk("gpio_str_proc_show something\n");
	return 0;
}

static int gpio_str_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, gpio_str_proc_show, NULL);
}

static ssize_t gpio_str_proc_read(struct file *file, char *buffer,
						size_t count, loff_t *pos)
{
	int i= 0 ;
	char buf2[21] = {0};
	if (count < 1)
		return -EINVAL;

	memset(buf2,0,sizeof(buf2));
	//gpio_lock(&gpio_lock);
	if(nr_gpios<20)
	{
		for(i=0;i<nr_gpios;i++)
		{
			buf2[i] = (gpio_get_value(gpios[i])== 1) ? '1':'0';	
			printk("gpio_str_proc_read %d is %c\n",gpios[i],buf2[i]);
		}
		buf2[i]  = '\0';
		if (copy_to_user(buffer, buf2, nr_gpios)) {
			//gpio_unlock(&gpio_lock); 		
			return -EFAULT;
		}
	}
	return 0;

}

static long gpio_unlock_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{	
	int ret = 0;		 
	printk("%s: cmd = %d arg = %ld\n",__func__,cmd, arg);	
	if(!gpio_is_valid(arg))
	{	
		printk("%s: gpio %ld is valid",__func__, arg);		
		return 0;
	}
	switch(cmd)	
	{		
		case 0:			
			gpio_direction_output(arg, 0);			
			break;		
		case 0x11:			
			gpio_direction_output(arg, 1);			
			break;		
		case 0x55:			
			gpio_direction_input(arg);			
			ret = gpio_get_value(arg);			
			break;		
		default:			
			printk("gpio_unlock_ioctl:unkown param cmd:%d\n",cmd);			
			break;	
	}	
	return ret;
}

static const struct file_operations gpio_extend_fops = {
	.owner		= THIS_MODULE,
	.open		= gpio_str_proc_open,
	.read		= gpio_str_proc_read,
	.llseek		= seq_lseek,
	.release	= single_release,
	.write		= gpio_write_proc_string,
	.unlocked_ioctl = gpio_unlock_ioctl,
};


static ssize_t pwm_store(struct device *dev, struct device_attribute *attr, const char *buf,size_t count)
{
      //  ssize_t ret = 0;
        unsigned long peroid =  simple_strtoul(buf,NULL,10);
        defalut_period = peroid;
        printk("%s...period = %ld\n",__func__,peroid);
        pwm_config(zed_pwm,peroid*100,pwm_period);

        return (ssize_t)count;
 
}



static ssize_t pwm_show(struct device *dev, struct device_attribute *attr, char *buf)
{
      //  ssize_t ret = 0;
        printk("%s...defalut_period = %ld\n",__func__,defalut_period);
        return  sprintf(buf,"%ld",defalut_period);

}



static DEVICE_ATTR(pwm, S_IWUSR | S_IRUSR , pwm_show, pwm_store);


static int gpio_control_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	enum of_gpio_flags flags;
	int i=0;
	int ret = 0;
	char gpio_name[100];
	//struct proc_dir_entry *ent;
	memset(gpio_name,0,sizeof(gpio_name));
	memset(gpios,0,sizeof(gpios));
	printk("cjc func: %s\n", __func__); 
	if (!node)
	return -ENODEV;

	printk("gpio_control_prob cjc #####\n");


	nr_gpios = of_gpio_named_count(node, "init-gpios");
	for(i=0;i<nr_gpios;i++)
	{
		sprintf(gpio_name,"gpio_control%d",i);
		gpios[i] = of_get_named_gpio_flags(node, "init-gpios", i, &flags);
		printk("%s:gpio_name = %s,%d\n",__func__,gpio_name,gpios[i]);
		if(!gpio_is_valid(gpios[i]))
		{		
			printk("------%s:valid %s gpio:%d-----\n",__func__,gpio_name,gpios[i]);	
			break;
		}
		else
		{	
			ret = gpio_request(gpios[i],gpio_name);
			if(ret<0)
			{
				printk("%s:gpio %s:%d request failed\n",__func__,gpio_name,gpios[i]);
			}
			gpio_direction_output(gpios[i],!(flags & OF_GPIO_ACTIVE_LOW));
		}
		memset(gpio_name,0,sizeof(gpio_name));
	}
//	gpio_request(91,"cam_rst");
//	gpio_direction_output(91,1);
/*
	gpio_extend_dir = proc_mkdir("gpio_extend", NULL);
	if(gpio_extend_dir == NULL)
	{
		printk("unable to creat /proc/gpio_extend directory\n");
		return -ENOMEM;
	}

	ent = proc_create("gpio_str", 0666, gpio_extend_dir, &gpio_extend_fops);
	if(ent == NULL)
	{
		printk("unable to create /proc/gpio_extend/gpio_str entry");
		//goto fail;
	}
*/

	zed_pwm = devm_pwm_get(&pdev->dev, NULL);
	if (IS_ERR(zed_pwm)) {
		printk("pwm unable to request PWM fan");
	} else {
		printk("pwm request fan  sucess ");
		pwm_adjust_config(zed_pwm);
		defalut_period = 50;
		pwm_period = pwm_get_period(zed_pwm);
		printk("pwm fan  period = %d n",pwm_period);
		pwm_config(zed_pwm,defalut_period*100,pwm_period);
		pwm_enable(zed_pwm);
		//debug节点   /sys/devices/platform/gpio_control/pwm
		ret = device_create_file(&pdev->dev, &dev_attr_pwm);    
	}										  
	rk_gpio_class = class_create(THIS_MODULE, "rk_gpios");
	major = register_chrdev(0, "rk_gpio", &gpio_extend_fops);
	device_create(rk_gpio_class, NULL, MKDEV(major, 0), NULL, "rk_gpio");  


	return 0;
//fail:
	printk("func: %s some error\n", __func__); 
	return 0;
}

static int gpio_control_remove(struct platform_device *pdev)
{
        //printk("func: %s\n", __func__);
    device_destroy(rk_gpio_class, MKDEV(major, 0));
	unregister_chrdev(major, "rk_gpio");
	class_destroy(rk_gpio_class);        
	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int gpio_control_suspend(struct device *dev)
{
        //printk("func: %s\n", __func__); 
	return 0;
}

static int gpio_control_resume(struct device *dev)
{
        //printk("func: %s\n", __func__); 
	return 0;
}
#endif

static const struct dev_pm_ops gpio_control_ops = {
#ifdef CONFIG_PM_SLEEP
	.suspend = gpio_control_suspend,
	.resume = gpio_control_resume,
	.poweroff = gpio_control_suspend,
	.restore = gpio_control_resume,
#endif
};

static struct platform_driver gpio_driver = {
	.driver		= {
		.name		= "gpio_control",
		.owner		= THIS_MODULE,
		.pm		= &gpio_control_ops,
		.of_match_table	= of_match_ptr(gpio_of_match),
	},
	.probe		= gpio_control_probe,
	.remove		= gpio_control_remove,
};

module_platform_driver(gpio_driver);

MODULE_DESCRIPTION("gpio_control");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:gpio_control");
