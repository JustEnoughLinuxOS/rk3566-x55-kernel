#include <linux/init.h>
#include <linux/input.h>
#include <linux/input-polldev.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of_device.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/workqueue.h>
#include <linux/uaccess.h>
#include <linux/iio/consumer.h>
#include <linux/iio/types.h>
#include <linux/iio/iio.h>
#include <linux/gpio.h>
#include <linux/of_irq.h>
#include <linux/of_gpio.h>

#if 1
	#define DBG(args...)  printk(args)
#endif

//键值可定义在DTS里面 比较方便修改
//tr: Key values can be defined in DTS, which is more convenient to modify.
static unsigned int joy_gpio_key_map[32] = {
											BTN_TR,
											BTN_TR2,
											BTN_TL,
											BTN_TL2,
											BTN_B,//BTN_A,
											BTN_A,//BTN_B,
											BTN_Y,//BTN_X,
											BTN_X,//BTN_Y,
											BTN_DPAD_LEFT,
											BTN_DPAD_RIGHT,
											BTN_DPAD_UP,
											BTN_DPAD_DOWN,
											BTN_START,//BTN_SELECT,
											KEY_VOLUMEDOWN,//BTN_START,
											BTN_SELECT,//KEY_VOLUMEUP,
											KEY_VOLUMEUP,//KEY_VOLUMEDOWN,
											BTN_THUMBL,
											BTN_THUMBR
											};

//adc
struct joy_adc_keys_button {
	u32 voltage;
	u32 keycode;
	u32 adc_chan;
	u32 last_key;
	struct delayed_work work;
};

//gpio
struct joy_gpio_keys_button {
	u32 gpio;
	u32 keycode;
	u32 irq;
	struct delayed_work work;
	struct gpio_desc *gpiod;
};

struct zed_joystick_dev {
	struct device *dev;
	struct input_polled_dev *poll_dev;
	struct input_dev *input_dev;
	struct iio_channel *channel0;
	struct iio_channel *channel1;
	struct iio_channel *channel2;
	struct iio_channel *channel3;
	u32 keyup_voltage;
	u32 num_keys;   //adc key
	u32 num_gpio_keys;   //gpio key
	u32 debounce_interval;
	u32 axis_level;
	u32 otg_gpio;  //推迟USB外设的使能 tr: Delay enabling of USB peripherals
	struct joy_adc_keys_button *map;
	struct joy_gpio_keys_button *gpio_map;
};

struct zed_joystick_dev *zed_joystick;
static u32 jitter = 30;

//音量加减对应的GPIO
//tr: GPIO corresponding to volume addition and subtraction
static u32 volume_up_gpio = 133;
static u32 volume_down_gpio = 131;



static void gpio_keys_gpio_work_func(struct work_struct *work)
{
	int state;	
	struct input_polled_dev *dev =  zed_joystick->poll_dev;	
	struct input_dev *input_dev =  zed_joystick->input_dev;	
	struct joy_gpio_keys_button *map = container_of(work, struct joy_gpio_keys_button, work.work);
	
	//低按下
	//tr: Active Low
	state = gpio_get_value(map->gpio);
	
	if ( map->gpio != volume_up_gpio &&  map->gpio != volume_down_gpio){//josytick设备 tr: Joystick Device
		input_report_key(dev->input, map->keycode, !state);
		input_sync(dev->input);
	}else{//kbd输入设备 tr: kbd input device
		input_report_key(input_dev, map->keycode, !state);
		input_sync(input_dev);
	}
}


static irqreturn_t keys_isr(int irq, void *dev_id)
{
	struct joy_gpio_keys_button * map = (struct joy_gpio_keys_button *)dev_id;

//	struct input_dev *input = zed_joystick->poll_dev->input;

	BUG_ON(irq != map->irq);
	
	//是否不需要使用延时 中断内不能有睡眠API
	//tr: Is there no need to use a delay? There cannot be a sleep API within the interrupt.
	//mod_delayed_work(system_wq,&map->work, msecs_to_jiffies(zed_joystick->debounce_interval));
	mod_delayed_work(system_wq,&map->work, msecs_to_jiffies(10));	

	return IRQ_HANDLED;
}


static void adc_keys_work_func(struct work_struct *work)
{
	//struct input_polled_dev *dev =  zed_joystick->poll_dev;	
	//struct joy_adc_keys_button *map = container_of(work, struct joy_adc_keys_button, work.work);
	
	//no-op?
}

//0~1800<------>-32768~32767
u32 adc_val_to_axis(int value)
{
	u32	axis_value;

	if ( (value < 855) || (value >= 950) ){
		axis_value = value/95;
	}else {
		axis_value = 9;
		
	}

	return axis_value;
}

static void jk_keys_poll(struct input_polled_dev *dev)
{
	struct zed_joystick_dev *jk = dev->private;
	int i, x, y, mag, ret;
	int deadzone, range = jk->keyup_voltage / 2;
	struct iio_channel *channels[] = {
		jk->channel0,
		jk->channel1,
		jk->channel2,
		jk->channel3,
	};

	// deadzone = range * 0.15
	deadzone = (range * 100) * 15 / 10000;

	for (i = 0; i < ARRAY_SIZE(channels); i+=2)
	{
		ret = iio_read_channel_processed(channels[i], &x);
		ret = iio_read_channel_processed(channels[i+1], &y);

		// Convert [0, max] to [-max/2, +max/2]
		x -= range;
		y -= range;

 		/* Joystick Deadzone check */
		mag = int_sqrt((x * x) + (y * y));
		if (deadzone) {
			if (mag <= deadzone) {
				x = 0;
				y = 0;
			}
			else {
				/* Assumes adc_max == -adcx->min == adcy->max == -adcy->min */
				/* Order of operations is critical to avoid integer overflow */
				x = (((range * x) / mag) * (mag - deadzone)) / (range - deadzone);
				y = (((range * y) / mag) * (mag - deadzone)) / (range - deadzone);
			}
 		}

		// Bias towards the outside by 5% (outer deadzone).
		y = (y * 100) * 105 / 10000;
		x = (x * 100) * 105 / 10000;

		// And now convert back
		x += range;
		y += range;

		input_report_abs(dev->input, jk->map[i].keycode, x);
		input_report_abs(dev->input, jk->map[i+1].keycode, y);
	}

	input_sync(dev->input);
}


static int adc_keys_load_keymap(struct device *dev, struct zed_joystick_dev *jk)
{
	struct joy_adc_keys_button *map;
	struct fwnode_handle *child;
	int i;
	
	jk->num_keys = device_get_child_node_count(dev);
	if (jk->num_keys == 0) {
		dev_err(dev, "keymap is missing\n");
		return -EINVAL;
	}
	
	map = devm_kmalloc_array(dev, jk->num_keys, sizeof(*map), GFP_KERNEL);
	if (!map)
		return -ENOMEM;
	
	i = 0;
	device_for_each_child_node(dev, child) {
		if (fwnode_property_read_u32(child, "press-threshold-microvolt",
					     &map[i].voltage)) {
			dev_err(dev, "Key with invalid or missing voltage\n");
			fwnode_handle_put(child);
			return -EINVAL;
		}
		map[i].voltage /= 1000;

		if (fwnode_property_read_u32(child, "linux,code",
					     &map[i].keycode)) {
			dev_err(dev, "Key with invalid or missing linux,code\n");
			fwnode_handle_put(child);
			return -EINVAL;
		}
		
		if (fwnode_property_read_u32(child, "adc-chan",
					     &map[i].adc_chan)) {
			dev_err(dev, "Key with invalid or missing adc-chan\n");
			fwnode_handle_put(child);
			return -EINVAL;
		}
		
		INIT_DELAYED_WORK(&map[i].work, adc_keys_work_func);
		
		
		DBG("%s keycode:voltage = %d:%d\n", __func__,map[i].keycode,map[i].voltage);
		
		i++;
	}

	jk->map = map;
	return 0;
	
}

static int gpio_keys_load_keymap(struct device *dev, struct zed_joystick_dev *jk)
{
	struct joy_gpio_keys_button *map;
	enum of_gpio_flags flags;
	char gpio_name[32];
	int i,ret = 0;
	
	struct device_node *node = dev->of_node;
	
	jk->num_gpio_keys = of_gpio_named_count(node, "key-gpios");
	
	DBG("%s Probing...gpio_keys num = %d \n",__func__,jk->num_gpio_keys);
	
	map = devm_kmalloc_array(dev, jk->num_gpio_keys, sizeof(*map), GFP_KERNEL);
	if (!map)
		return -ENOMEM;
	
	for (i = 0; i < jk->num_gpio_keys; i++){
		map[i].gpio = of_get_named_gpio_flags(node, "key-gpios", i, &flags);
		map[i].keycode = joy_gpio_key_map[i];
		sprintf(gpio_name,"joy_gpio%d",i);
		
		if (!gpio_is_valid(map[i].gpio)){
            DBG("%s:invalid gpio \n",__func__);
            break;
		} else {
			DBG("%s:request gpio = %d \n",__func__,map[i].gpio);
			ret = gpio_request(map[i].gpio,gpio_name);
			if ( ret<0 ){
				printk("%s:gpio %s:%d request failed\n",__func__,gpio_name,map[i].gpio);
            }
			
			ret = gpio_direction_input(map[i].gpio);
			if (ret < 0) {
				pr_err("gpio-keys: failed to configure input direction for GPIO %d, error %d\n",
				       map[i].gpio, ret);
				gpio_free(map[i].gpio);
			}
			
			map[i].irq = gpio_to_irq(map[i].gpio);
			if (map[i].irq < 0) {
				ret = map[i].irq;
				pr_err("gpio-keys: Unable to get irq number for GPIO %d, error %d\n",
				       map[i].gpio, ret);
				gpio_free(map[i].gpio);
			}
			
			map[i].gpiod = gpio_to_desc(map[i].gpio);
			
			if (!map[i].gpiod)
				return -EINVAL;
			
			if (map[i].gpiod) {
				if (jk->debounce_interval) 
						ret = gpiod_set_debounce(map[i].gpiod,jk->debounce_interval * 1000);
					
				if (ret < 0)
					DBG("%s gpio Not Supported .debounce..\n",__func__);
				
			}
			
			INIT_DELAYED_WORK(&map[i].work, gpio_keys_gpio_work_func);
			
			ret = devm_request_irq(dev, map[i].irq, keys_isr,IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING, "keys", &map[i]);
			if (ret) {
				pr_err("gpio-keys: Unable to claim irq %d; error %d\n",
				       map[i].irq, ret);
				gpio_free(map[i].gpio);
			}
		}
	}
	jk->gpio_map = map;
	
	return ret;
}

static int zed_joystick_probe(struct platform_device *pdev)
{
	struct device_node *np;
	struct input_polled_dev *poll_dev;
	struct input_dev *input_dev;
	struct input_dev *input_dev1;
	enum iio_chan_type type;
	int value, error;
	int channel;
	int range;
	int fuzz, deadzone;
	enum of_gpio_flags flags;

	DBG("%s Probing...\n",__func__);
	np = pdev->dev.of_node;
	zed_joystick = devm_kzalloc(&pdev->dev,sizeof(struct zed_joystick_dev), GFP_KERNEL);
	platform_set_drvdata(pdev, zed_joystick);
	dev_set_drvdata(&pdev->dev, zed_joystick);
	zed_joystick->dev = &pdev->dev;
	
	//adc 0
	zed_joystick->channel0 = devm_iio_channel_get(zed_joystick->dev, "button0");
	if (IS_ERR(zed_joystick->channel0))
		return PTR_ERR(zed_joystick->channel0);
	if (!zed_joystick->channel0->indio_dev)
		return -ENXIO;
	
	error = iio_get_channel_type(zed_joystick->channel0, &type);
	if (error < 0)
		return error;

	if (type != IIO_VOLTAGE) {
		dev_err(zed_joystick->dev, "Incompatible channel type %d\n", type);
		return -EINVAL;
	}
	
	channel = zed_joystick->channel0->channel->channel;
	DBG("%s Probing...channel0 id %d\n",__func__,channel);
	
	//adc 1
	zed_joystick->channel1 = devm_iio_channel_get(zed_joystick->dev, "button1");
	if (IS_ERR(zed_joystick->channel1))
		return PTR_ERR(zed_joystick->channel1);
	if (!zed_joystick->channel1->indio_dev)
		return -ENXIO;
	error = iio_get_channel_type(zed_joystick->channel1, &type);
	if (error < 0)
		return error;

	if (type != IIO_VOLTAGE) {
		dev_err(zed_joystick->dev, "Incompatible channel type %d\n", type);
		return -EINVAL;
	}
	channel = zed_joystick->channel1->channel->channel;
	DBG("%s Probing...channel1 id %d\n",__func__,channel);
	
	//adc 2
	zed_joystick->channel2 = devm_iio_channel_get(zed_joystick->dev, "button2");
	if (IS_ERR(zed_joystick->channel2))
		return PTR_ERR(zed_joystick->channel2);
	if (!zed_joystick->channel2->indio_dev)
		return -ENXIO;
	error = iio_get_channel_type(zed_joystick->channel2, &type);
	if (error < 0)
		return error;

	if (type != IIO_VOLTAGE) {
		dev_err(zed_joystick->dev, "Incompatible channel type %d\n", type);
		return -EINVAL;
	}
	channel = zed_joystick->channel2->channel->channel;
	DBG("%s Probing...channel2 id %d\n",__func__,channel);
	
	//adc 3
	zed_joystick->channel3 = devm_iio_channel_get(zed_joystick->dev, "button3");
	if (IS_ERR(zed_joystick->channel3))
		return PTR_ERR(zed_joystick->channel3);
	if (!zed_joystick->channel3->indio_dev)
		return -ENXIO;
	error = iio_get_channel_type(zed_joystick->channel3, &type);
	if (error < 0)
		return error;

	if (type != IIO_VOLTAGE) {
		dev_err(zed_joystick->dev, "Incompatible channel type %d\n", type);
		return -EINVAL;
	}
	channel = zed_joystick->channel3->channel->channel;
	DBG("%s Probing...channel3 id %d\n",__func__,channel);
	
	if (device_property_read_u32(zed_joystick->dev, "keyup-threshold-microvolt",
				     &zed_joystick->keyup_voltage)) {
		dev_err(zed_joystick->dev, "Invalid or missing keyup voltage\n");
		return -EINVAL;
	}

	zed_joystick->keyup_voltage /= 1000;
	range = zed_joystick->keyup_voltage;
	
	// Johnny: Was this used for anything? I don't see anything regarding this
	// even on the original tree
	zed_joystick->axis_level = zed_joystick->keyup_voltage/jitter;
	DBG("%s Probing... axis_level %d\n",__func__,zed_joystick->axis_level);
	
	error = adc_keys_load_keymap(zed_joystick->dev, zed_joystick);
	if (error)
		return error;
	
	error = gpio_keys_load_keymap(zed_joystick->dev, zed_joystick);
	if (error)
		return error;
	
	if (!device_property_read_u32(zed_joystick->dev, "debounce-interval", &value))
		zed_joystick->debounce_interval = value;
	
	DBG("%s Probing...debounce_interval = %d \n",__func__,zed_joystick->debounce_interval);
	
	poll_dev = devm_input_allocate_polled_device(zed_joystick->dev);
	if (!poll_dev) {
		dev_err(zed_joystick->dev, "failed to allocate input device\n");
		return -ENOMEM;
	}
	
	if (!device_property_read_u32(zed_joystick->dev, "poll-interval", &value))
		poll_dev->poll_interval = value;

	DBG("%s Probing...poll_interval = %d \n",__func__,poll_dev->poll_interval);

	poll_dev->poll = jk_keys_poll;
	poll_dev->private = zed_joystick;
	
	input_dev = poll_dev->input;
	
	zed_joystick->poll_dev = poll_dev;

	input_dev->evbit[0] = BIT(EV_KEY) | BIT(EV_REP) | BIT(EV_ABS);
	input_dev->name = pdev->name;
	input_dev->dev.parent = &pdev->dev;
	input_dev->id.bustype = BUS_HOST;

//	__set_bit(KEY_POWER,input_dev->keybit);
	__set_bit(BTN_DPAD_RIGHT,input_dev->keybit);
	__set_bit(BTN_DPAD_LEFT,input_dev->keybit);
	__set_bit(BTN_DPAD_DOWN,input_dev->keybit);
	__set_bit(BTN_DPAD_UP,input_dev->keybit);
//	__set_bit(KEY_VOLUMEDOWN,input_dev->keybit);
//	__set_bit(KEY_VOLUMEUP,input_dev->keybit);
	__set_bit(BTN_A,input_dev->keybit);
	__set_bit(BTN_B,input_dev->keybit);
	__set_bit(BTN_X,input_dev->keybit);
	__set_bit(BTN_Y,input_dev->keybit);
	__set_bit(BTN_TL,input_dev->keybit);
	__set_bit(BTN_TR,input_dev->keybit);
	__set_bit(BTN_TL2,input_dev->keybit);
	__set_bit(BTN_TR2,input_dev->keybit);
	__set_bit(BTN_SELECT,input_dev->keybit);
	__set_bit(BTN_START,input_dev->keybit);
	__set_bit(BTN_MODE,input_dev->keybit);
	__set_bit(BTN_THUMBL,input_dev->keybit);  //leftstick
	__set_bit(BTN_THUMBR,input_dev->keybit);  //rightstick
	__set_bit(BTN_GAMEPAD,input_dev->keybit);
	
	//不映射  -32768  32767
	//tr: Do not map as -32768  32767 (johnny: because this is from the adc readings?)
	fuzz = ((range / 2) * 100) * 5 / 10000; // 5% fuzz
	deadzone = ((range / 2) * 100) * 15 / 10000; // 15% deadzone
	input_set_abs_params(input_dev, ABS_X, 0, range, fuzz, deadzone);
	input_set_abs_params(input_dev, ABS_Y, 0, range, fuzz, deadzone);
	input_set_abs_params(input_dev, ABS_RX, 0, range, fuzz, deadzone);
	input_set_abs_params(input_dev, ABS_RY, 0, range, fuzz, deadzone);

	error = input_register_polled_device(poll_dev);
	if (error) {
		dev_err(zed_joystick->dev, "Unable to register input device: %d\n", error);
		return error;
	}

	//kbd输入设备
	input_dev1 = devm_input_allocate_device(zed_joystick->dev);
	if (!input_dev1) {
		dev_err(zed_joystick->dev, "failed to allocate input_dev1 device\n");
		return -ENOMEM;
	}
	
	zed_joystick->input_dev = input_dev1;
	input_dev1->evbit[0] = BIT(EV_KEY) | BIT(EV_REP);
	input_dev1->name = "zed_keyboard";
	input_dev1->dev.parent = &pdev->dev;
	input_dev1->id.bustype = BUS_HOST;
	
	__set_bit(KEY_VOLUMEDOWN,input_dev1->keybit);
	__set_bit(KEY_VOLUMEUP,input_dev1->keybit);
	
	error = input_register_device(input_dev1);
	if (error) {
		dev_err(zed_joystick->dev, "Unable to register input_dev1, error: %d\n",
			error);
		return error;
	}

	zed_joystick->otg_gpio = of_get_named_gpio_flags(np, "otg-gpio", 0, &flags);
	if ( !gpio_is_valid(zed_joystick->otg_gpio) ) {
            DBG("%s:invalid gpio \n",__func__);
           
	} else {	
		error = gpio_request(zed_joystick->otg_gpio,"otg_gpio");
		if ( error<0 ){
			DBG("%s:gpio %d request failed\n",__func__,zed_joystick->otg_gpio);
		}
			
		error = gpio_direction_output(zed_joystick->otg_gpio,1);
		if (error < 0) {
				pr_err("gpio-keys: failed to configureotg_gpio %d, error %d\n",
				       zed_joystick->otg_gpio, error);
				gpio_free(zed_joystick->otg_gpio);
		}
	}

	DBG("%s Probing...end\n",__func__);
	return 0;
}

static int zed_joystick_remove(struct platform_device *pdev)
{
	return 0;
}

static int zed_joystick_suspend(struct platform_device *pdev,pm_message_t state)
{
	return 0;
}

static int zed_joystick_resume(struct platform_device *pdev)
{
	return 0;
}

static void zed_joystick_shutdown(struct platform_device *pdev)
{

	return ;
}

static const struct of_device_id zed_joystick_of_match[] = {
	{.compatible = "zed,joystick",},

};

static struct platform_driver zed_joystick_driver = {
	.driver = {
		.name = "zed_joystick",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(zed_joystick_of_match),
	},
	.probe = zed_joystick_probe,
	.remove = zed_joystick_remove,
	.suspend = zed_joystick_suspend,
	.resume = zed_joystick_resume,
	.shutdown = zed_joystick_shutdown,
};

module_platform_driver(zed_joystick_driver)


MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("zed joystick drvier");
MODULE_AUTHOR("lx/x_liu@zediel.com");


