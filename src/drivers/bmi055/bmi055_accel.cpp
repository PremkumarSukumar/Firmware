/**
 * Copyright (C) 2015 - 2016 Bosch Sensortec GmbH
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * Neither the name of the copyright holder nor the names of the
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL COPYRIGHT HOLDER
 * OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY,
 * OR CONSEQUENTIAL DAMAGES(INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE
 *
 * The information provided is believed to be accurate and reliable.
 * The copyright holder assumes no responsibility
 * for the consequences of use
 * of such information nor for any infringement of patents or
 * other rights of third parties which may result from its use.
 * No license is granted by implication or otherwise under any patent or
 * patent rights of the copyright holder.
 *
 */

#include "bmi055.hpp"


/*
  list of registers that will be checked in check_registers(). Note
  that ADDR_WHO_AM_I must be first in the list.
 */
const uint8_t BMI055_accel::_checked_registers[BMI055_ACCEL_NUM_CHECKED_REGISTERS] = {    BMI055_ACC_CHIP_ID,
                                          BMI055_ACC_BW,
									      BMI055_ACC_RANGE,
									      BMI055_ACC_INT_EN_1,
									      BMI055_ACC_INT_MAP_1,
									 };



BMI055_accel::BMI055_accel(int bus, const char *path_accel, spi_dev_e device, enum Rotation rotation) :
    SPI("BMI055", path_accel, bus, device, SPIDEV_MODE3, BMI055_BUS_SPEED),
    _whoami(0),
    _call{},
    _call_interval(0),
    _accel_reports(nullptr),
    _accel_scale{},
    _accel_range_scale(0.0f),
    _accel_range_m_s2(0.0f),
    _accel_topic(nullptr),
    _accel_orb_class_instance(-1),
    _accel_class_instance(-1),
     _dlpf_freq(0),
    _accel_sample_rate(BMI055_ACCEL_DEFAULT_RATE),
    _accel_reads(perf_alloc(PC_COUNT, "bmi055_accel_read")),
    _sample_perf(perf_alloc(PC_ELAPSED, "bmi055_read")),
    _bad_transfers(perf_alloc(PC_COUNT, "bmi055_bad_transfers")),
    _bad_registers(perf_alloc(PC_COUNT, "bmi055_bad_registers")),
    _good_transfers(perf_alloc(PC_COUNT, "bmi055_good_transfers")),
    _reset_retries(perf_alloc(PC_COUNT, "bmi055_reset_retries")),
    _duplicates(perf_alloc(PC_COUNT, "bmi055_duplicates")),
    _controller_latency_perf(perf_alloc_once(PC_ELAPSED, "ctrl_latency")),
    _register_wait(0),
    _reset_wait(0),
    _accel_filter_x(BMI055_ACCEL_DEFAULT_RATE, BMI055_ACCEL_DEFAULT_DRIVER_FILTER_FREQ),
    _accel_filter_y(BMI055_ACCEL_DEFAULT_RATE, BMI055_ACCEL_DEFAULT_DRIVER_FILTER_FREQ),
    _accel_filter_z(BMI055_ACCEL_DEFAULT_RATE, BMI055_ACCEL_DEFAULT_DRIVER_FILTER_FREQ),
    _accel_int(1000000 / BMI055_ACCEL_MAX_PUBLISH_RATE),
    _rotation(rotation),
    _checked_next(0),
    _last_temperature(0),
    _last_accel{},
    _got_duplicate(false)
{
    // disable debug() calls
    _debug_enabled = false;

    _device_id.devid_s.devtype = DRV_ACC_DEVTYPE_BMI160;

    // default accel scale factors
    _accel_scale.x_offset = 0;
    _accel_scale.x_scale  = 1.0f;
    _accel_scale.y_offset = 0;
    _accel_scale.y_scale  = 1.0f;
    _accel_scale.z_offset = 0;
    _accel_scale.z_scale  = 1.0f;

    memset(&_call, 0, sizeof(_call));
}


BMI055_accel::~BMI055_accel()
{
    /* make sure we are truly inactive */
    stop();

    /* free any existing reports */
    if (_accel_reports != nullptr) {
        delete _accel_reports;
    }


    if (_accel_class_instance != -1) {
        unregister_class_devname(ACCEL_BASE_DEVICE_PATH, _accel_class_instance);
    }

    /* delete the perf counter */
    perf_free(_sample_perf);
    perf_free(_accel_reads);
    perf_free(_bad_transfers);
    perf_free(_bad_registers);
    perf_free(_good_transfers);
    perf_free(_reset_retries);
    perf_free(_duplicates);
}


int
BMI055_accel::init()
{
    int ret;

    /* do SPI init (and probe) first */
    ret = SPI::init();

    /* if probe/setup failed, bail now */
    if (ret != OK) {
        warnx("SPI error");
        DEVICE_DEBUG("SPI setup failed");
        return ret;
    }

    /* allocate basic report buffers */
    _accel_reports = new ringbuffer::RingBuffer(2, sizeof(accel_report));

    if (_accel_reports == nullptr) {
        goto out;
    }

    if (reset() != OK) {
        goto out;
    }

    /* Initialize offsets and scales */
    _accel_scale.x_offset = 0;
    _accel_scale.x_scale  = 1.0f;
    _accel_scale.y_offset = 0;
    _accel_scale.y_scale  = 1.0f;
    _accel_scale.z_offset = 0;
    _accel_scale.z_scale  = 1.0f;


    _accel_class_instance = register_class_devname(ACCEL_BASE_DEVICE_PATH);

    measure();

    /* advertise sensor topic, measure manually to initialize valid report */
    struct accel_report arp;
    _accel_reports->get(&arp);

    /* measurement will have generated a report, publish */
    _accel_topic = orb_advertise_multi(ORB_ID(sensor_accel), &arp,
                       &_accel_orb_class_instance, (is_external()) ? ORB_PRIO_MAX - 1 : ORB_PRIO_HIGH - 1);

    if (_accel_topic == nullptr) {
        warnx("ADVERT FAIL");
    }

out:
    return ret;
}

int BMI055_accel::reset()
{
    write_reg(BMI055_ACC_SOFTRESET, BMI055_SOFT_RESET);//Soft-reset
    up_udelay(5000);

    write_checked_reg(BMI055_ACC_BW,    BMI055_ACCEL_RATE_1000); //Write bandwidth
    write_checked_reg(BMI055_ACC_RANGE,     BMI055_ACCEL_RANGE_2_G);//Write range
    write_checked_reg(BMI055_ACC_INT_EN_1,      BMI055_ACC_DRDY_INT_EN); //Enable DRDY interrupt
    write_checked_reg(BMI055_ACC_INT_MAP_1,     BMI055_ACC_DRDY_INT1); //DRDY interrupt on pin INT1

    set_accel_range(BMI055_ACCEL_DEFAULT_RANGE_G);//set accel range
    accel_set_sample_rate(BMI055_ACCEL_DEFAULT_RATE);//set accel ODR

    //Enable Accelerometer in normal mode
    write_reg(BMI055_ACC_PMU_LPW, BMI055_ACCEL_NORMAL);
    up_udelay(1000);

    uint8_t retries = 10;

    while (retries--) {
        bool all_ok = true;

        for (uint8_t i = 0; i < BMI055_ACCEL_NUM_CHECKED_REGISTERS; i++) {
            if (read_reg(_checked_registers[i]) != _checked_values[i]) {
                write_reg(_checked_registers[i], _checked_values[i]);
                all_ok = false;
            }
        }

        if (all_ok) {
            break;
        }
    }

    _accel_reads = 0;

    return OK;
}


int
BMI055_accel::probe()
{
	/* look for device ID */
	_whoami = read_reg(BMI055_ACC_CHIP_ID);
	// verify product revision
	switch (_whoami) {
	case BMI055_ACC_WHO_AM_I:
		memset(_checked_values, 0, sizeof(_checked_values));
		memset(_checked_bad, 0, sizeof(_checked_bad));
		_checked_values[0] = _whoami;
		_checked_bad[0] = _whoami;
		return OK;
	}

	DEVICE_DEBUG("unexpected whoami 0x%02x", _whoami);
	return -EIO;
}



int
BMI055_accel::accel_set_sample_rate(float frequency)
{
	uint8_t setbits = 0;
	uint8_t clearbits = (BMI055_ACCEL_RATE_7_81 | BMI055_ACCEL_RATE_1000);


	if (frequency <= (781 / 100)) {
		setbits |= BMI055_ACCEL_RATE_7_81;
		_accel_sample_rate = 781 / 100;

	} else if (frequency <= (1563 / 100)) {
		setbits |= BMI055_ACCEL_RATE_15_63;
		_accel_sample_rate = 1563 / 100;

	} else if (frequency <= (3125 / 100)) {
		setbits |= BMI055_ACCEL_RATE_31_25;
		_accel_sample_rate = 3125 / 100;

	} else if (frequency <= (625 / 10)) {
		setbits |= BMI055_ACCEL_RATE_62_5;
		_accel_sample_rate = 625 / 10;

	} else if (frequency <= 125) {
		setbits |= BMI055_ACCEL_RATE_125;
		_accel_sample_rate = 125;

	} else if (frequency <= 250) {
		setbits |= BMI055_ACCEL_RATE_250;
		_accel_sample_rate = 250;

	} else if (frequency <= 500) {
		setbits |= BMI055_ACCEL_RATE_500;
		_accel_sample_rate = 500;

	} else if (frequency <= 1000) {
		setbits |= BMI055_ACCEL_RATE_1000;
		_accel_sample_rate = 1000;

	} else if (frequency > 1000) {
		setbits |= BMI055_ACCEL_RATE_1000;
		_accel_sample_rate = 1000;

	} else {
		return -EINVAL;
	}

	modify_reg(BMI055_ACC_BW, clearbits, setbits);

	return OK;
}


void
BMI055_accel::_set_dlpf_filter(uint16_t bandwidth)
{
	_dlpf_freq = 0;
	bandwidth = bandwidth;   //TO BE IMPLEMENTED
	/*uint8_t setbits = BW_SCAL_ODR_BW_XL;
	uint8_t clearbits = BW_XL_50_HZ;

	if (bandwidth == 0) {
	    _dlpf_freq = 408;
	    clearbits = BW_SCAL_ODR_BW_XL | BW_XL_50_HZ;
	    setbits = 0;
	}

	if (bandwidth <= 50) {
	    setbits |= BW_XL_50_HZ;
	    _dlpf_freq = 50;

	} else if (bandwidth <= 105) {
	    setbits |= BW_XL_105_HZ;
	    _dlpf_freq = 105;

	} else if (bandwidth <= 211) {
	    setbits |= BW_XL_211_HZ;
	    _dlpf_freq = 211;

	} else if (bandwidth <= 408) {
	    setbits |= BW_XL_408_HZ;
	    _dlpf_freq = 408;

	}
	modify_reg(CTRL_REG6_XL, clearbits, setbits);*/
}

ssize_t
BMI055_accel::read(struct file *filp, char *buffer, size_t buflen)
{
	unsigned count = buflen / sizeof(accel_report);

	/* buffer must be large enough */
	if (count < 1) {
		return -ENOSPC;
	}

	/* if automatic measurement is not enabled, get a fresh measurement into the buffer */
	if (_call_interval == 0) {
		_accel_reports->flush();
		measure();
	}

	/* if no data, error (we could block here) */
	if (_accel_reports->empty()) {
		return -EAGAIN;
	}

	perf_count(_accel_reads);

	/* copy reports out of our buffer to the caller */
	accel_report *arp = reinterpret_cast<accel_report *>(buffer);
	int transferred = 0;

	while (count--) {
		if (!_accel_reports->get(arp)) {
			break;
		}

		transferred++;
		arp++;
	}

	/* return the number of bytes transferred */
	return (transferred * sizeof(accel_report));
}

int
BMI055_accel::self_test()
{
	if (perf_event_count(_sample_perf) == 0) {
		measure();
	}

	/* return 0 on success, 1 else */
	return (perf_event_count(_sample_perf) > 0) ? 0 : 1;
}

int
BMI055_accel::accel_self_test()
{
	if (self_test()) {
		return 1;
	}

	/* inspect accel offsets */
	if (fabsf(_accel_scale.x_offset) < 0.000001f) {
		return 1;
	}

	if (fabsf(_accel_scale.x_scale - 1.0f) > 0.4f || fabsf(_accel_scale.x_scale - 1.0f) < 0.000001f) {
		return 1;
	}

	if (fabsf(_accel_scale.y_offset) < 0.000001f) {
		return 1;
	}

	if (fabsf(_accel_scale.y_scale - 1.0f) > 0.4f || fabsf(_accel_scale.y_scale - 1.0f) < 0.000001f) {
		return 1;
	}

	if (fabsf(_accel_scale.z_offset) < 0.000001f) {
		return 1;
	}

	if (fabsf(_accel_scale.z_scale - 1.0f) > 0.4f || fabsf(_accel_scale.z_scale - 1.0f) < 0.000001f) {
		return 1;
	}

	return 0;
}


/*
  deliberately trigger an error in the sensor to trigger recovery
 */
void
BMI055_accel::test_error()
{
	write_reg(BMI055_ACC_SOFTRESET, BMI055_SOFT_RESET);
	::printf("error triggered\n");
	print_registers();
}


int
BMI055_accel::ioctl(struct file *filp, int cmd, unsigned long arg)
{
    switch (cmd) {

    case SENSORIOCRESET:
        return reset();

    case SENSORIOCSPOLLRATE: {
            switch (arg) {

            /* switching to manual polling */
            case SENSOR_POLLRATE_MANUAL:
                stop();
                _call_interval = 0;
                return OK;

            /* external signalling not supported */
            case SENSOR_POLLRATE_EXTERNAL:

            /* zero would be bad */
            case 0:
                return -EINVAL;

            /* set default/max polling rate */
            case SENSOR_POLLRATE_MAX:
                return ioctl(filp, SENSORIOCSPOLLRATE, BMI055_ACCEL_MAX_RATE);

            case SENSOR_POLLRATE_DEFAULT:
                    return ioctl(filp, SENSORIOCSPOLLRATE, BMI055_ACCEL_DEFAULT_RATE); //Polling at the highest frequency. We may get duplicate values on the sensors

            /* adjust to a legal polling interval in Hz */
            default: {
                    /* do we need to start internal polling? */
                    bool want_start = (_call_interval == 0);

                    /* convert hz to hrt interval via microseconds */
                    unsigned ticks = 1000000 / arg;

                    /* check against maximum sane rate */
                    if (ticks < 1000) {
                        return -EINVAL;
                    }

                    // adjust filters
                    float cutoff_freq_hz = _accel_filter_x.get_cutoff_freq();
                    float sample_rate = 1.0e6f / ticks;
                    _set_dlpf_filter(cutoff_freq_hz);
                    _accel_filter_x.set_cutoff_frequency(sample_rate, cutoff_freq_hz);
                    _accel_filter_y.set_cutoff_frequency(sample_rate, cutoff_freq_hz);
                    _accel_filter_z.set_cutoff_frequency(sample_rate, cutoff_freq_hz);

                    /* update interval for next measurement */
                    /* XXX this is a bit shady, but no other way to adjust... */
                    _call_interval = ticks;

                    /*
                      set call interval faster then the sample time. We
                      then detect when we have duplicate samples and reject
                      them. This prevents aliasing due to a beat between the
                      stm32 clock and the bmi055 clock
                     */
                    _call.period = _call_interval - BMI055_TIMER_REDUCTION;

                    /* if we need to start the poll state machine, do it */
                    if (want_start) {
                        start();
                    }

                    return OK;
                }
            }
        }

    case SENSORIOCGPOLLRATE:
        if (_call_interval == 0) {
            return SENSOR_POLLRATE_MANUAL;
        }

        return 1000000 / _call_interval;

    case SENSORIOCSQUEUEDEPTH: {
            /* lower bound is mandatory, upper bound is a sanity check */
            if ((arg < 1) || (arg > 100)) {
                return -EINVAL;
            }

            irqstate_t flags = px4_enter_critical_section();

            if (!_accel_reports->resize(arg)) {
                px4_leave_critical_section(flags);
                return -ENOMEM;
            }

            px4_leave_critical_section(flags);

            return OK;
        }

    case SENSORIOCGQUEUEDEPTH:
        return _accel_reports->size();

    case ACCELIOCGSAMPLERATE:
        return _accel_sample_rate;

    case ACCELIOCSSAMPLERATE:
        return accel_set_sample_rate(arg);

    case ACCELIOCGLOWPASS:
        return _accel_filter_x.get_cutoff_freq();

    case ACCELIOCSLOWPASS:
        // set software filtering
        _accel_filter_x.set_cutoff_frequency(1.0e6f / _call_interval, arg);
        _accel_filter_y.set_cutoff_frequency(1.0e6f / _call_interval, arg);
        _accel_filter_z.set_cutoff_frequency(1.0e6f / _call_interval, arg);
        return OK;

    case ACCELIOCSSCALE: {
            /* copy scale, but only if off by a few percent */
            struct accel_calibration_s *s = (struct accel_calibration_s *) arg;
            float sum = s->x_scale + s->y_scale + s->z_scale;

            if (sum > 2.0f && sum < 4.0f) {
                memcpy(&_accel_scale, s, sizeof(_accel_scale));
                return OK;

            } else {
                return -EINVAL;
            }
        }

    case ACCELIOCGSCALE:
        /* copy scale out */
        memcpy((struct accel_calibration_s *) arg, &_accel_scale, sizeof(_accel_scale));
        return OK;

    case ACCELIOCSRANGE:
        return set_accel_range(arg);

    case ACCELIOCGRANGE:
        return (unsigned long)((_accel_range_m_s2) / BMI055_ONE_G + 0.5f);

    case ACCELIOCSELFTEST:
        return accel_self_test();

#ifdef ACCELIOCSHWLOWPASS

    case ACCELIOCSHWLOWPASS:
        _set_dlpf_filter(arg);
        return OK;
#endif

#ifdef ACCELIOCGHWLOWPASS

    case ACCELIOCGHWLOWPASS:
        return _dlpf_freq;
#endif


    default:
        /* give it to the superclass */
        return SPI::ioctl(filp, cmd, arg);
    }
}


uint8_t
BMI055_accel::read_reg(unsigned reg)
{
	uint8_t cmd[2] = { (uint8_t)(reg | DIR_READ), 0};

	transfer(cmd, cmd, sizeof(cmd));

	return cmd[1];
}

uint16_t
BMI055_accel::read_reg16(unsigned reg)
{
	uint8_t cmd[3] = { (uint8_t)(reg | DIR_READ), 0, 0 };

	transfer(cmd, cmd, sizeof(cmd));

	return (uint16_t)(cmd[1] << 8) | cmd[2];
}

void
BMI055_accel::write_reg(unsigned reg, uint8_t value)
{
	uint8_t	cmd[2];

	cmd[0] = reg | DIR_WRITE;
	cmd[1] = value;

	transfer(cmd, nullptr, sizeof(cmd));
}

void
BMI055_accel::modify_reg(unsigned reg, uint8_t clearbits, uint8_t setbits)
{
	uint8_t	val;

	val = read_reg(reg);
	val &= ~clearbits;
	val |= setbits;
	write_checked_reg(reg, val);
}

void
BMI055_accel::write_checked_reg(unsigned reg, uint8_t value)
{
	write_reg(reg, value);

	for (uint8_t i = 0; i < BMI055_ACCEL_NUM_CHECKED_REGISTERS; i++) {
		if (reg == _checked_registers[i]) {
			_checked_values[i] = value;
			_checked_bad[i] = value;
		}
	}
}

int
BMI055_accel::set_accel_range(unsigned max_g)
{
	uint8_t setbits = 0;
	uint8_t clearbits = BMI055_ACCEL_RANGE_2_G | BMI055_ACCEL_RANGE_16_G;
	float lsb_per_g;
	float max_accel_g;

	if (max_g == 0) {
		max_g = 16;
	}

	if (max_g <= 2) {
		max_accel_g = 2;
		setbits |= BMI055_ACCEL_RANGE_2_G;
                lsb_per_g = 1024;

	} else if (max_g <= 4) {
		max_accel_g = 4;
		setbits |= BMI055_ACCEL_RANGE_4_G;
                lsb_per_g = 512;

	} else if (max_g <= 8) {
		max_accel_g = 8;
		setbits |= BMI055_ACCEL_RANGE_8_G;
                lsb_per_g = 256;

	} else if (max_g <= 16) {
		max_accel_g = 16;
		setbits |= BMI055_ACCEL_RANGE_16_G;
                lsb_per_g = 128;

	} else {
		return -EINVAL;
	}

	_accel_range_scale = (BMI055_ONE_G / lsb_per_g);
	_accel_range_m_s2 = max_accel_g * BMI055_ONE_G;

	modify_reg(BMI055_ACC_RANGE, clearbits, setbits);

	return OK;
}


void
BMI055_accel::start()
{
    /* make sure we are stopped first */
    stop();

    /* discard any stale data in the buffers */
    _accel_reports->flush();

    /* start polling at the specified rate */
    hrt_call_every(&_call,
               1000,
               _call_interval - BMI055_TIMER_REDUCTION,
               (hrt_callout)&BMI055_accel::measure_trampoline, this);
    reset();
}

void
BMI055_accel::stop()
{
	hrt_cancel(&_call);
}

void
BMI055_accel::measure_trampoline(void *arg)
{
	BMI055_accel *dev = reinterpret_cast<BMI055_accel *>(arg);

	/* make another measurement */
	dev->measure();
}

void
BMI055_accel::check_registers(void)
{
	uint8_t v;

	if ((v = read_reg(_checked_registers[_checked_next])) !=
	    _checked_values[_checked_next]) {
		_checked_bad[_checked_next] = v;

		/*
		  if we get the wrong value then we know the SPI bus
		  or sensor is very sick. We set _register_wait to 20
		  and wait until we have seen 20 good values in a row
		  before we consider the sensor to be OK again.
		 */
		perf_count(_bad_registers);

		/*
		  try to fix the bad register value. We only try to
		  fix one per loop to prevent a bad sensor hogging the
		  bus.
		 */
		if (_register_wait == 0 || _checked_next == 0) {
			// if the product_id is wrong then reset the
			// sensor completely
			write_reg(BMI055_ACC_SOFTRESET, BMI055_SOFT_RESET);
			_reset_wait = hrt_absolute_time() + 10000;
			_checked_next = 0;

		} else {
			write_reg(_checked_registers[_checked_next], _checked_values[_checked_next]);
			// waiting 3ms between register writes seems
			// to raise the chance of the sensor
			// recovering considerably
			_reset_wait = hrt_absolute_time() + 3000;
		}

		_register_wait = 20;
	}

	_checked_next = (_checked_next + 1) % BMI055_ACCEL_NUM_CHECKED_REGISTERS;
}


void
BMI055_accel::measure()
{
    uint8_t index = 0, data_array[7];
    uint16_t lsb, msb, msblsb;

    if (hrt_absolute_time() < _reset_wait) {
        // we're waiting for a reset to complete
        return;
    }

    struct BMI_AccelReport bmi_accel_report;

    struct Report {
        int16_t     accel_x;
        int16_t     accel_y;
        int16_t     accel_z;
        int16_t     temp;
    } report;

    /* start measuring */
    perf_begin(_sample_perf);

    /*
     * Fetch the full set of measurements from the BMI055 in one pass.
     */
    bmi_accel_report.cmd = BMI055_ACC_X_L | DIR_READ;
    data_array[index] = BMI055_ACC_X_L | DIR_READ;

    if (OK != transfer(data_array, data_array, sizeof(data_array))) {
        return;
    }

    check_registers();

    index = 1;
    lsb = (uint16_t)data_array[index++];
    msb = (uint16_t)data_array[index++];
    msblsb = (msb << 8) | lsb;
    bmi_accel_report.accel_x = ((int16_t)msblsb >> 4); /* Data in X axis */

    lsb = (uint16_t)data_array[index++];
    msb = (uint16_t)data_array[index++];
    msblsb = (msb << 8) | lsb;
    bmi_accel_report.accel_y = ((int16_t)msblsb >> 4); /* Data in Y axis */

    lsb = (uint16_t)data_array[index++];
    msb = (uint16_t)data_array[index++];
    msblsb = (msb << 8) | lsb;
    bmi_accel_report.accel_z = ((int16_t)msblsb >> 4); /* Data in Z axis */


    _last_accel[0] = bmi_accel_report.accel_x;
    _last_accel[1] = bmi_accel_report.accel_y;
    _last_accel[2] = bmi_accel_report.accel_z;
    _got_duplicate = false;

    uint8_t temp = read_reg(BMI055_ACC_TEMP);
    report.temp = temp;


    report.accel_x = bmi_accel_report.accel_x;
    report.accel_y = bmi_accel_report.accel_y;
    report.accel_z = bmi_accel_report.accel_z;

    if (report.accel_x == 0 &&
        report.accel_y == 0 &&
        report.accel_z == 0 &&
        report.temp == 0) {
        // all zero data - probably a SPI bus error
        perf_count(_bad_transfers);
        perf_end(_sample_perf);
        // note that we don't call reset() here as a reset()
        // costs 20ms with interrupts disabled. That means if
        // the bmi055 does go bad it would cause a FMU failure,
        // regardless of whether another sensor is available,
        return;
    }


    perf_count(_good_transfers);

    if (_register_wait != 0) {
        // we are waiting for some good transfers before using
        // the sensor again. We still increment
        // _good_transfers, but don't return any data yet
        _register_wait--;
        return;
    }

    /*
     * Report buffers.
     */
    accel_report        arb;

    /*
     * Adjust and scale results to m/s^2.
     */
//    grb.timestamp = arb.timestamp = hrt_absolute_time();//Prem
    arb.timestamp = hrt_absolute_time();


    // report the error count as the sum of the number of bad
    // transfers and bad register reads. This allows the higher
    // level code to decide if it should use this sensor based on
    // whether it has had failures
//    grb.error_count = arb.error_count = perf_event_count(_bad_transfers) + perf_event_count(_bad_registers);//Prem
    arb.error_count = perf_event_count(_bad_transfers) + perf_event_count(_bad_registers);//Prem
    /*
     * 1) Scale raw value to SI units using scaling from datasheet.
     * 2) Subtract static offset (in SI units)
     * 3) Scale the statically calibrated values with a linear
     *    dynamically obtained factor
     *
     * Note: the static sensor offset is the number the sensor outputs
     *   at a nominally 'zero' input. Therefore the offset has to
     *   be subtracted.
     *
     *   Example: A gyro outputs a value of 74 at zero angular rate
     *        the offset is 74 from the origin and subtracting
     *        74 from all measurements centers them around zero.
     */


    /* NOTE: Axes have been swapped to match the board a few lines above. */

    arb.x_raw = report.accel_x;
    arb.y_raw = report.accel_y;
    arb.z_raw = report.accel_z;

    float xraw_f = report.accel_x;
    float yraw_f = report.accel_y;
    float zraw_f = report.accel_z;

    // apply user specified rotation
    rotate_3f(_rotation, xraw_f, yraw_f, zraw_f);

    float x_in_new = ((xraw_f * _accel_range_scale) - _accel_scale.x_offset) * _accel_scale.x_scale;
    float y_in_new = ((yraw_f * _accel_range_scale) - _accel_scale.y_offset) * _accel_scale.y_scale;
    float z_in_new = ((zraw_f * _accel_range_scale) - _accel_scale.z_offset) * _accel_scale.z_scale;

    arb.x = _accel_filter_x.apply(x_in_new);
    arb.y = _accel_filter_y.apply(y_in_new);
    arb.z = _accel_filter_z.apply(z_in_new);

    math::Vector<3> aval(x_in_new, y_in_new, z_in_new);
    math::Vector<3> aval_integrated;

    bool accel_notify = _accel_int.put(arb.timestamp, aval, aval_integrated, arb.integral_dt);
    arb.x_integral = aval_integrated(0);
    arb.y_integral = aval_integrated(1);
    arb.z_integral = aval_integrated(2);

    arb.scaling = _accel_range_scale;
    arb.range_m_s2 = _accel_range_m_s2;

    _last_temperature = 23 + report.temp * 1.0f / 512.0f;

    arb.temperature_raw = report.temp;
    arb.temperature = _last_temperature;

    _accel_reports->force(&arb);

    /* notify anyone waiting for data */
    if (accel_notify) {
        poll_notify(POLLIN);
    }

    if (accel_notify && !(_pub_blocked)) {
        /* log the time of this report */
        perf_begin(_controller_latency_perf);
        /* publish it */
        orb_publish(ORB_ID(sensor_accel), _accel_topic, &arb);
    }
    /* stop measuring */
    perf_end(_sample_perf);
}


void
BMI055_accel::print_info()
{
    warnx("BMI055 Accel");
    perf_print_counter(_sample_perf);
    perf_print_counter(_accel_reads);
    perf_print_counter(_bad_transfers);
    perf_print_counter(_bad_registers);
    perf_print_counter(_good_transfers);
    perf_print_counter(_reset_retries);
    perf_print_counter(_duplicates);
    _accel_reports->print_info("accel queue");
    ::printf("checked_next: %u\n", _checked_next);

    for (uint8_t i = 0; i < BMI055_ACCEL_NUM_CHECKED_REGISTERS; i++) {
        uint8_t v = read_reg(_checked_registers[i]);

        if (v != _checked_values[i]) {
            ::printf("reg %02x:%02x should be %02x\n",
                 (unsigned)_checked_registers[i],
                 (unsigned)v,
                 (unsigned)_checked_values[i]);
        }

        if (v != _checked_bad[i]) {
            ::printf("reg %02x:%02x was bad %02x\n",
                 (unsigned)_checked_registers[i],
                 (unsigned)v,
                 (unsigned)_checked_bad[i]);
        }
    }

    ::printf("temperature: %.1f\n", (double)_last_temperature);
    printf("\n");
}


void
BMI055_accel::print_registers()
{
    uint8_t index = 0;
	printf("BMI055 accel registers\n");

    uint8_t reg = _checked_registers[index++];
    uint8_t v = read_reg(reg);
    printf("Accel Chip Id: %02x:%02x ", (unsigned)reg, (unsigned)v);
    printf("\n");

    reg = _checked_registers[index++];
    v = read_reg(reg);
    printf("Accel Bw: %02x:%02x ", (unsigned)reg, (unsigned)v);
    printf("\n");

    reg = _checked_registers[index++];
    v = read_reg(reg);
    printf("Accel Range: %02x:%02x ", (unsigned)reg, (unsigned)v);
    printf("\n");

    reg = _checked_registers[index++];
    v = read_reg(reg);
    printf("Accel Int-en-1: %02x:%02x ", (unsigned)reg, (unsigned)v);
    printf("\n");

    reg = _checked_registers[index++];
    v = read_reg(reg);
    printf("Accel Int-Map-1: %02x:%02x ", (unsigned)reg, (unsigned)v);

    printf("\n");
}


