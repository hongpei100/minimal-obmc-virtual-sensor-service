// SPDX-License-Identifier: GPL-2.0
/*
 * vtemp.c - Virtual temperature sensor exposed through the hwmon subsystem.
 *
 * Provides a fake temperature chip with:
 *   temp1_input      (RO)  current temperature, milli-degree Celsius
 *   temp1_max        (RW)  over-temperature limit, milli-degree Celsius
 *   temp1_max_alarm  (RO)  1 when temp1_input >= temp1_max
 *   temp_inject      (RW)  driver-private attribute: write here to change
 *                          the temperature seen by user space
 *
 * The device is instantiated in module_init() via a platform_device,
 * since there is no real hardware and no device tree node to match.
 */

 #include <linux/module.h>
 #include <linux/platform_device.h>
 #include <linux/hwmon.h>
 #include <linux/mutex.h>
 #include <linux/err.h>
 
 #define VTEMP_DEFAULT_TEMP	45000	/* 45.0 degC */
 #define VTEMP_DEFAULT_MAX	75000	/* 75.0 degC */
 
 struct vtemp_data {
     struct mutex lock;	/* protects temp and temp_max */
     long temp;		/* milli-degree Celsius */
     long temp_max;		/* milli-degree Celsius */
 };
 
 /* ---------------------------------------------------------------------
  * hwmon core callbacks: is_visible / read / write
  * ------------------------------------------------------------------- */
 
 static umode_t vtemp_is_visible(const void *drvdata,
                 enum hwmon_sensor_types type,
                 u32 attr, int channel)
 {
     if (type != hwmon_temp)
         return 0;
 
     switch (attr) {
     case hwmon_temp_input:
     case hwmon_temp_max_alarm:
         return 0444;
     case hwmon_temp_max:
         return 0644;
     default:
         return 0;
     }
 }
 
 static int vtemp_read(struct device *dev, enum hwmon_sensor_types type,
               u32 attr, int channel, long *val)
 {
     struct vtemp_data *data = dev_get_drvdata(dev);
 
     if (type != hwmon_temp)
         return -EOPNOTSUPP;
 
     mutex_lock(&data->lock);
     switch (attr) {
     case hwmon_temp_input:
         *val = data->temp;
         break;
     case hwmon_temp_max:
         *val = data->temp_max;
         break;
     case hwmon_temp_max_alarm:
         *val = (data->temp >= data->temp_max) ? 1 : 0;
         break;
     default:
         mutex_unlock(&data->lock);
         return -EOPNOTSUPP;
     }
     mutex_unlock(&data->lock);
 
     return 0;
 }
 
 static int vtemp_write(struct device *dev, enum hwmon_sensor_types type,
                u32 attr, int channel, long val)
 {
     struct vtemp_data *data = dev_get_drvdata(dev);
 
     if (type != hwmon_temp || attr != hwmon_temp_max)
         return -EOPNOTSUPP;
 
     /* keep the limit in a sane range: -40 degC .. +125 degC */
     val = clamp_val(val, -40000, 125000);
 
     mutex_lock(&data->lock);
     data->temp_max = val;
     mutex_unlock(&data->lock);
 
     return 0;
 }
 
 static const struct hwmon_ops vtemp_hwmon_ops = {
     .is_visible = vtemp_is_visible,
     .read = vtemp_read,
     .write = vtemp_write,
 };
 
 static const struct hwmon_channel_info * const vtemp_info[] = {
     HWMON_CHANNEL_INFO(temp,
                HWMON_T_INPUT | HWMON_T_MAX | HWMON_T_MAX_ALARM),
     NULL
 };
 
 static const struct hwmon_chip_info vtemp_chip_info = {
     .ops = &vtemp_hwmon_ops,
     .info = vtemp_info,
 };
 
 /* ---------------------------------------------------------------------
  * Driver-private attribute: temp_inject
  *
  * This is NOT part of the hwmon standard API, so it is registered as an
  * "extra group" alongside the standard attributes. User space (or a test
  * script) writes a milli-degree value here to simulate the sensor moving.
  * ------------------------------------------------------------------- */
 
 static ssize_t temp_inject_show(struct device *dev,
                 struct device_attribute *attr, char *buf)
 {
     struct vtemp_data *data = dev_get_drvdata(dev);
     long v;
 
     mutex_lock(&data->lock);
     v = data->temp;
     mutex_unlock(&data->lock);
 
     return sysfs_emit(buf, "%ld\n", v);
 }
 
 static ssize_t temp_inject_store(struct device *dev,
                  struct device_attribute *attr,
                  const char *buf, size_t count)
 {
     struct vtemp_data *data = dev_get_drvdata(dev);
     long v;
     int ret;
 
     ret = kstrtol(buf, 10, &v);
     if (ret)
         return ret;
 
     v = clamp_val(v, -40000, 125000);
 
     mutex_lock(&data->lock);
     data->temp = v;
     mutex_unlock(&data->lock);
 
     return count;
 }
 
 static DEVICE_ATTR_RW(temp_inject);
 
 static struct attribute *vtemp_extra_attrs[] = {
     &dev_attr_temp_inject.attr,
     NULL
 };
 ATTRIBUTE_GROUPS(vtemp_extra);
 
 /* ---------------------------------------------------------------------
  * Platform driver glue
  * ------------------------------------------------------------------- */
 
 static int vtemp_probe(struct platform_device *pdev)
 {
     struct device *dev = &pdev->dev;
     struct vtemp_data *data;
     struct device *hwmon_dev;
 
     data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
     if (!data)
         return -ENOMEM;
 
     mutex_init(&data->lock);
     data->temp = VTEMP_DEFAULT_TEMP;
     data->temp_max = VTEMP_DEFAULT_MAX;
 
     hwmon_dev = devm_hwmon_device_register_with_info(dev, "vtemp",
                              data,
                              &vtemp_chip_info,
                              vtemp_extra_groups);
     if (IS_ERR(hwmon_dev))
         return PTR_ERR(hwmon_dev);
 
     dev_info(dev, "vtemp hwmon device registered\n");
     return 0;
 }
 
 static struct platform_driver vtemp_driver = {
     .driver = {
         .name = "vtemp",
     },
     .probe = vtemp_probe,
     /* no .remove needed: everything is devm-managed */
 };
 
 static struct platform_device *vtemp_pdev;
 
 static int __init vtemp_init(void)
 {
     int ret;
 
     ret = platform_driver_register(&vtemp_driver);
     if (ret)
         return ret;
 
     /*
      * No firmware (device tree / ACPI) describes this device, so we
      * create the platform_device by hand. The name "vtemp" matches
      * vtemp_driver.driver.name, which is what triggers probe().
      */
     vtemp_pdev = platform_device_register_simple("vtemp", -1, NULL, 0);
     if (IS_ERR(vtemp_pdev)) {
         platform_driver_unregister(&vtemp_driver);
         return PTR_ERR(vtemp_pdev);
     }
 
     return 0;
 }
 
 static void __exit vtemp_exit(void)
 {
     platform_device_unregister(vtemp_pdev);
     platform_driver_unregister(&vtemp_driver);
 }
 
 module_init(vtemp_init);
 module_exit(vtemp_exit);
 
 MODULE_AUTHOR("Leo");
 MODULE_DESCRIPTION("Virtual temperature sensor (hwmon) with injectable value");
 MODULE_LICENSE("GPL");