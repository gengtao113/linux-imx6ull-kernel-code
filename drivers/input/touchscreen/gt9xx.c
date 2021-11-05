/***************************************************************
 Copyright © ALIENTEK Co., Ltd. 1998-2029. All rights reserved.
 文件名    : gt9xx.c
 作者      : 邓涛
 版本      : V1.0
 描述      : GOODiX GT9147/GT9271/GT1151触摸屏驱动程序
 其他      : 无
 论坛      : www.openedv.com
 日志      : 初版V1.0 2020/7/26 邓涛创建
 ***************************************************************/

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/input/mt.h>
#include <linux/of_gpio.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/of_device.h>
#include <asm/unaligned.h>

/* 寄存器定义 */
#define GOODIX_REG_COMMAND		0x8040
#define GOODIX_REG_CONFIG_DATA	0x8047
#define GOODIX_REG_ID			0x8140
#define GOODIX_READ_COOR_ADDR	0x814E

static const unsigned int goodix_irq_flags[] = {
	IRQF_TRIGGER_RISING,	//上升沿
	IRQF_TRIGGER_FALLING,	//下降沿
	IRQF_TRIGGER_LOW,		//低电平
	IRQF_TRIGGER_HIGH,		//高电平
};

struct goodix_ts_dev {
	struct i2c_client *client;
	struct input_dev *input;
	int max_support_points;		//支持的最大触摸点数
	int rst_gpio;				//复位引脚
	int irq_gpio;				//中断引脚
	int abs_x_max;				//X轴最大值
	int abs_y_max;				//Y轴最大值
	u16 id;
	u16 version;
	unsigned int irq_flag;		//中断触发方式
};

static int goodix_i2c_write(struct i2c_client *client,
			u16 addr, u8 *buf, int len)
{
	struct i2c_msg msg = {0};
	u8 w_buf[256];
	int ret;

	w_buf[0] = addr >> 8;
	w_buf[1] = addr & 0xFF;
	memcpy(&w_buf[2], buf, len);

	msg.flags = 0;			//i2c写
	msg.addr = client->addr;
	msg.buf = w_buf;
	msg.len = len + 2;

	ret = i2c_transfer(client->adapter, &msg, 1);
	if (1 != ret) {
		//dev_err(&client->dev, "%s error: addr=0x%x, len=%d\n",
		//			__func__, addr, len);
		return -1;
	}

	return 0;
}

static int goodix_i2c_read(struct i2c_client *client,
			u16 addr, u8 *buf, int len)
{
	struct i2c_msg msg[2];
	u8 w_buf[2];
	int ret;

	w_buf[0] = addr >> 8;
	w_buf[1] = addr & 0xFF;

	msg[0].flags = 0;			// i2c写
	msg[0].addr = client->addr;
	msg[0].buf = w_buf;
	msg[0].len = 2;				// 2个字节

	msg[1].flags = I2C_M_RD;	//i2c读
	msg[1].addr = client->addr;
	msg[1].buf = buf;
	msg[1].len = len;

	ret = i2c_transfer(client->adapter, msg, 2);
	if (2 != ret) {
		//dev_err(&client->dev, "%s error: addr=0x%x, len=%d\n",
		//			__func__, addr, len);
		return -1;
	}

	return 0;
}

static int goodix_ts_reset(struct goodix_ts_dev *ts)
{
	struct i2c_client *client = ts->client;
	int ret;

	/* 从设备树中获取复位管脚和中断管脚 */
	ts->rst_gpio = of_get_named_gpio(client->dev.of_node, "goodix,rst-gpio", 0);
	if (!gpio_is_valid(ts->rst_gpio)) {
		dev_err(&client->dev, "Failed to get reset gpio\n");
		return -1;
	}

	ts->irq_gpio = of_get_named_gpio(client->dev.of_node, "goodix,irq-gpio", 0);
	if (!gpio_is_valid(ts->irq_gpio)) {
		dev_err(&client->dev, "Failed to get IRQ gpio\n");
		return -1;
	}

	/* 申请使用管脚 */
	ret = devm_gpio_request_one(&client->dev, ts->rst_gpio,
				GPIOF_OUT_INIT_HIGH, "Goodix-Ts reset PIN");
	if (ret < 0)
		return ret;

	ret = devm_gpio_request_one(&client->dev, ts->irq_gpio,
				GPIOF_OUT_INIT_HIGH, "Goodix-Ts IRQ PIN");
	if (ret < 0)
		return ret;

	/*
	 *硬件复位开始
	 *这里严格按照官方参考手册提供的复位时序
	 */
	msleep(5);

	/* begin select I2C slave addr */
	gpio_set_value_cansleep(ts->rst_gpio, 0);
	msleep(20);					/* T2: > 10ms */

	/* HIGH: 0x28/0x29, LOW: 0xBA/0xBB */
	gpio_set_value_cansleep(ts->irq_gpio, client->addr == 0x14);
	usleep_range(200, 1000);	/* T3: > 100us */
	gpio_set_value_cansleep(ts->rst_gpio, 1);
	msleep(10);					/* T4: > 5ms */

	/* end select I2C slave addr */
	gpio_direction_input(ts->rst_gpio);

	/* 中断管脚拉低 */
	gpio_set_value_cansleep(ts->irq_gpio, 0);
	msleep(50);					/* T5: 50ms */

	/* 将中断引脚设置为输入模式 */
	gpio_direction_input(ts->irq_gpio);
	return 0;
}

static int goodix_software_reset(struct goodix_ts_dev *ts)
{
	u8 cmd;
	int ret;

	cmd = 0x02;		//差值原始值
	ret = goodix_i2c_write(ts->client, GOODIX_REG_COMMAND, &cmd, 1); /* 软复位 */
	if (ret)
		return ret;
	msleep(50);

	cmd = 0x0;		//读坐标状态
	ret = goodix_i2c_write(ts->client, GOODIX_REG_COMMAND, &cmd, 1); /* 停止软复位 */
	if (ret)
		return ret;
	msleep(50);

	return 0;
}

static int goodix_read_version(struct goodix_ts_dev *ts)
{
	int ret;
	u8 buf[6];

	ret = goodix_i2c_read(ts->client, GOODIX_REG_ID, buf, sizeof(buf));
	if (ret)
		return ret;

	/* 得到version */
	ts->version = get_unaligned_le16(&buf[4]);

	/* 得到ID */
	buf[4] = 0;
	if (kstrtou16(buf, 10, &ts->id))
		ts->id = 0x1001;	// 如果转换失败、则使用默认值0x1001

	/* 打印信息 */
	dev_info(&ts->client->dev, "ID: %d, version: %04x\n",
				ts->id, ts->version);
	return 0;
}

static int goodix_read_cfg(struct goodix_ts_dev *ts)
{
	int ret;
	u8 buf[7];

	ret = goodix_i2c_read(ts->client, GOODIX_REG_CONFIG_DATA, buf, sizeof(buf));
	if (ret)
		return ret;

	ts->max_support_points = buf[5] & 0x0F;
	ts->irq_flag = goodix_irq_flags[buf[6] & 0x03];
	ts->abs_x_max = buf[1] | (buf[2] << 8);
	ts->abs_y_max = buf[3] | (buf[4] << 8);
	if (1158 == ts->id) {
		ts->max_support_points = 5;
		ts->irq_flag = goodix_irq_flags[1];
		ts->abs_x_max = 800;
		ts->abs_y_max = 480;
	}

	dev_info(&ts->client->dev, "touch number: %d\n", ts->max_support_points);
	dev_info(&ts->client->dev, "irq trigger: %d\n", ts->irq_flag);
	dev_info(&ts->client->dev, "abs_x: %d, abs_y: %d\n", ts->abs_x_max, ts->abs_y_max);

	return 0;
}

static irqreturn_t goodix_ts_isr(int irq, void *dev_id)
{
	struct goodix_ts_dev *ts = dev_id;
	u8 data[1 + 8 * ts->max_support_points];
	int ids[ts->max_support_points];
	int touch_num;
	u8 clear = 0x0;
	int ret;
	int i, j;
	static int last_touch_num = 0;
	static int last_ids[10];

	/* 读取触摸数据 */
	ret = goodix_i2c_read(ts->client, GOODIX_READ_COOR_ADDR, data, 8 + 1);
	if (ret)
		return IRQ_HANDLED;

	/* 判断数据是否准备好 */
	if (0 == (data[0] & 0x80))
		return IRQ_HANDLED;

	/* 获取触摸点数 */
	touch_num = data[0] & 0x0F;
	if (touch_num > ts->max_support_points)
		goto out;

	/* 读取触摸数据 */
	if (touch_num > 1) {
		ret = goodix_i2c_read(ts->client,
					GOODIX_READ_COOR_ADDR + 1 + 8,
					&data[1 + 8],
					8 * (touch_num - 1));
		if (ret)
			goto out;
	}

	/* 上报按下数据 */
	for (i = 0; i < touch_num; i++) {
		int id, x, y;
		int index = i * 8 + 1;

		id = data[index] & 0x0F;
		x = get_unaligned_le16(&data[index + 1]);
		y = get_unaligned_le16(&data[index + 3]);

		input_mt_slot(ts->input, id);
		input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, true);//按下
		input_report_abs(ts->input, ABS_MT_POSITION_X, x);
		input_report_abs(ts->input, ABS_MT_POSITION_Y, y);
		ids[i] = id;
	}

	/* 上报触摸屏松开相关事件 */
	for (i = 0; i < last_touch_num; ) {

		for (j = 0; j < touch_num; j++) {

			if (last_ids[i] == ids[j])
				goto next;
		}

		input_mt_slot(ts->input, last_ids[i]);
		input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, false);//松开
next:
		i++;
	}

	/* 同步 */
	input_mt_report_pointer_emulation(ts->input, true);
	input_sync(ts->input);

	/* 重置last_ids和last_touch_num */
	for (i = 0; i < touch_num; i++)
		last_ids[i] = ids[i];
	last_touch_num = touch_num;

out:
	goodix_i2c_write(ts->client, GOODIX_READ_COOR_ADDR, &clear, 1);//清除中断标志
	return IRQ_HANDLED;
}

static int goodix_ts_irq(struct goodix_ts_dev *ts)
{
	struct i2c_client *client = ts->client;

	/* 注册中断服务函数 */
	return devm_request_threaded_irq(&client->dev, client->irq,
				NULL, goodix_ts_isr, ts->irq_flag | IRQF_ONESHOT,
				client->name, ts);
}

static int goodix_ts_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct goodix_ts_dev *ts = NULL;
	struct input_dev *input = NULL;
	u8 clear = 0x0;
	int ret;

	/* 实例化一个struct goodix_ts_dev对象 */
	ts = devm_kzalloc(&client->dev, sizeof(struct goodix_ts_dev), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	ts->client = client;

	/* 复位触摸芯片 */
	ret = goodix_ts_reset(ts);
	if (ret)
		return ret;

	msleep(5);

	/* 软件复位 */
	ret = goodix_software_reset(ts);
	if (ret)
		return ret;

	/* 读取版本信息 */
	ret = goodix_read_version(ts);
	if (ret)
		return ret;

	/* 读取配置信息 */
	ret = goodix_read_cfg(ts);
	if (ret)
		return ret;

	/* 注册input设备 */
	input = devm_input_allocate_device(&client->dev);
	if (!input)
		return -ENOMEM;

	input->name = "Goodix Capacitive TouchScreen";
	input->id.bustype = BUS_I2C;
	input->phys = "input/ts";
	input->id.vendor = 0x0416;
	input->id.product = ts->id;
	input->id.version = ts->version;
	ts->input = input;

	input_set_abs_params(input, ABS_MT_POSITION_X,
				0, ts->abs_x_max, 0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_Y,
				0, ts->abs_y_max, 0, 0);
	input_mt_init_slots(input, ts->max_support_points,
				INPUT_MT_DIRECT);

	ret = input_register_device(input);//注册
	if (ret)
		return ret;

	/* 注册中断服务函数 */
	ret = goodix_i2c_write(ts->client, GOODIX_READ_COOR_ADDR, &clear, 1);//清除中断标志
	if (ret) {
		input_unregister_device(ts->input);
		return ret;
	}

	ret = goodix_ts_irq(ts);	//注册中断
	if (ret) {
		input_unregister_device(ts->input);
		return ret;
	}

	/* 设备唤醒初始化 */
	device_init_wakeup(&client->dev, 1);

	/* 将ts与client进行绑定 */
	i2c_set_clientdata(client, ts);
	return 0;
}

static int goodix_ts_remove(struct i2c_client *client)
{
	struct goodix_ts_dev *ts = i2c_get_clientdata(client);
	input_unregister_device(ts->input);
	return 0;
}

static int __maybe_unused goodix_ts_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);

	if (device_may_wakeup(dev))
		enable_irq_wake(client->irq);

	return 0;
}

static int __maybe_unused goodix_ts_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);

	if (device_may_wakeup(dev))
		disable_irq_wake(client->irq);

	return 0;
}

static SIMPLE_DEV_PM_OPS(goodix_ts_pm_ops,
			goodix_ts_suspend, goodix_ts_resume);

static const struct i2c_device_id goodix_ts_id[] = {
	{ "goodix-ts", 0 },
	{ "goodix-gtxx", 0 },
	{ "goodix-gt9147", 0 },
	{ "goodix-gt9271", 0 },
	{ "goodix-gt1151", 0 },
	{ }
};

#ifdef CONFIG_OF
static const struct of_device_id goodix_of_match[] = {
	{ .compatible = "goodix,ts", },
	{ .compatible = "goodix,gtxx", },
	{ .compatible = "goodix,gt9147" },
	{ .compatible = "goodix,gt9271" },
	{ .compatible = "goodix,gt1151" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, goodix_of_match);
#endif

static struct i2c_driver goodix_ts_driver = {
	.probe    = goodix_ts_probe,
	.remove   = goodix_ts_remove,
	.id_table = goodix_ts_id,
	.driver = {
		.owner			= THIS_MODULE,
		.name			= "goodix_gtxx",
		.of_match_table	= of_match_ptr(goodix_of_match),
		.pm 			= &goodix_ts_pm_ops,
	},
};
module_i2c_driver(goodix_ts_driver);

MODULE_AUTHOR("Deng Tao <773904075@qq.com>, ALIENTEK, Inc.");
MODULE_DESCRIPTION("Goodix touchscreen driver");
MODULE_LICENSE("GPL v2");
