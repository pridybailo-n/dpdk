/*-
 *   BSD LICENSE
 *
 *   Copyright 2016 NXP.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of NXP nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <string.h>
#include <dirent.h>

#include <rte_log.h>
#include <rte_bus.h>
#include <rte_eal_memconfig.h>
#include <rte_malloc.h>
#include <rte_devargs.h>
#include <rte_memcpy.h>
#include <rte_ethdev.h>

#include <rte_fslmc.h>
#include <fslmc_vfio.h>

#define FSLMC_BUS_LOG(level, fmt, args...) \
	RTE_LOG(level, EAL, "%s(): " fmt "\n", __func__, ##args)

#define VFIO_IOMMU_GROUP_PATH "/sys/kernel/iommu_groups"

struct rte_fslmc_bus rte_fslmc_bus;

static void
cleanup_fslmc_device_list(void)
{
	struct rte_dpaa2_device *dev;
	struct rte_dpaa2_device *t_dev;

	TAILQ_FOREACH_SAFE(dev, &rte_fslmc_bus.device_list, next, t_dev) {
		TAILQ_REMOVE(&rte_fslmc_bus.device_list, dev, next);
		free(dev);
		dev = NULL;
	}
}

static int
scan_one_fslmc_device(char *dev_name)
{
	char *dup_dev_name, *t_ptr;
	struct rte_dpaa2_device *dev;

	if (!dev_name)
		return -1;

	/* Ignore the Container name itself */
	if (!strncmp("dprc", dev_name, 4))
		return 0;

	/* Creating a temporary copy to perform cut-parse over string */
	dup_dev_name = strdup(dev_name);
	if (!dup_dev_name) {
		FSLMC_BUS_LOG(ERR, "Out of memory.");
		return -ENOMEM;
	}

	/* For all other devices, we allocate rte_dpaa2_device.
	 * For those devices where there is no driver, probe would release
	 * the memory associated with the rte_dpaa2_device after necessary
	 * initialization.
	 */
	dev = calloc(1, sizeof(struct rte_dpaa2_device));
	if (!dev) {
		FSLMC_BUS_LOG(ERR, "Out of memory.");
		free(dup_dev_name);
		return -ENOMEM;
	}

	/* Parse the device name and ID */
	t_ptr = strtok(dup_dev_name, ".");
	if (!t_ptr) {
		FSLMC_BUS_LOG(ERR, "Incorrect device string observed.");
		goto cleanup;
	}
	if (!strncmp("dpni", t_ptr, 4))
		dev->dev_type = DPAA2_ETH;
	else if (!strncmp("dpseci", t_ptr, 6))
		dev->dev_type = DPAA2_CRYPTO;
	else if (!strncmp("dpcon", t_ptr, 5))
		dev->dev_type = DPAA2_CON;
	else if (!strncmp("dpbp", t_ptr, 4))
		dev->dev_type = DPAA2_BPOOL;
	else if (!strncmp("dpio", t_ptr, 4))
		dev->dev_type = DPAA2_IO;
	else if (!strncmp("dpci", t_ptr, 5))
		dev->dev_type = DPAA2_CI;
	else if (!strncmp("dpmcp", t_ptr, 5))
		dev->dev_type = DPAA2_MPORTAL;
	else
		dev->dev_type = DPAA2_UNKNOWN;

	t_ptr = strtok(NULL, ".");
	if (!t_ptr) {
		FSLMC_BUS_LOG(ERR, "Incorrect device string observed (%s).",
			      t_ptr);
		goto cleanup;
	}

	sscanf(t_ptr, "%hu", &dev->object_id);
	dev->device.name = strdup(dev_name);
	if (!dev->device.name) {
		FSLMC_BUS_LOG(ERR, "Out of memory.");
		goto cleanup;
	}

	/* Add device in the fslmc device list */
	TAILQ_INSERT_TAIL(&rte_fslmc_bus.device_list, dev, next);
	if (dev->dev_type == DPAA2_ETH)
		RTE_LOG(INFO, EAL, "%s: Eth Device scanned.\n",
			dev->device.name);
	if (dev->dev_type == DPAA2_CRYPTO)
		RTE_LOG(INFO, EAL, "%s: Crypto Device scanned.\n",
			dev->device.name);
	RTE_LOG(DEBUG, EAL, "Add device (%s) with object ID: %d, Type = %d\n",
		dev->device.name, dev->object_id, dev->dev_type);

	/* Don't need the duplicated device filesystem entry anymore */
	if (dup_dev_name)
		free(dup_dev_name);

	return 0;
cleanup:
	if (dup_dev_name)
		free(dup_dev_name);
	if (dev)
		free(dev);
	return -1;
}

static int
rte_fslmc_scan(void)
{
	int ret;
	int device_count = 0;
	char fslmc_dirpath[PATH_MAX];
	DIR *dir;
	struct dirent *entry;
	static int process_once;
	int groupid;

	if (process_once) {
		FSLMC_BUS_LOG(DEBUG,
			      "Fslmc bus already scanned. Not rescanning");
		return 0;
	}
	process_once = 1;

	ret = fslmc_get_container_group(&groupid);
	if (ret != 0)
		goto scan_fail;

	/* Scan devices on the group */
	sprintf(fslmc_dirpath, "%s/%d/devices", VFIO_IOMMU_GROUP_PATH,
		groupid);
	dir = opendir(fslmc_dirpath);
	if (!dir) {
		FSLMC_BUS_LOG(ERR, "Unable to open VFIO group dir.");
		goto scan_fail;
	}

	while ((entry = readdir(dir)) != NULL) {
		if (entry->d_name[0] == '.' || entry->d_type != DT_LNK)
			continue;

		ret = scan_one_fslmc_device(entry->d_name);
		if (ret != 0) {
			/* Error in parsing directory - exit gracefully */
			goto scan_fail_cleanup;
		}
		device_count += 1;
	}

	RTE_LOG(INFO, EAL, "fslmc: Bus scan completed\n");

	closedir(dir);
	return 0;

scan_fail_cleanup:
	closedir(dir);

	/* Remove all devices in the list */
	cleanup_fslmc_device_list();
scan_fail:
	RTE_LOG(DEBUG, EAL, "Error in scanning FSLMC Bus. Skipping.\n");
	/* Irrespective of failure, scan only return success */
	return 0;
}

static int
rte_fslmc_match(struct rte_dpaa2_driver *dpaa2_drv,
		struct rte_dpaa2_device *dpaa2_dev)
{
	if (dpaa2_drv->drv_type == dpaa2_dev->dev_type)
		return 0;

	return 1;
}

static int
rte_fslmc_probe(void)
{
	int ret = 0;
	struct rte_dpaa2_device *dev;
	struct rte_dpaa2_driver *drv;

	ret = fslmc_vfio_setup_group();
	if (ret) {
		FSLMC_BUS_LOG(ERR, "Unable to setup VFIO");
		goto probe_fail;
	}

	ret = fslmc_vfio_process_group();
	if (ret) {
		FSLMC_BUS_LOG(ERR, "Unable to setup devices");
		goto probe_fail;
	}

	TAILQ_FOREACH(dev, &rte_fslmc_bus.device_list, next) {
		TAILQ_FOREACH(drv, &rte_fslmc_bus.driver_list, next) {
			ret = rte_fslmc_match(drv, dev);
			if (ret)
				continue;

			if (!drv->probe)
				continue;

			ret = drv->probe(drv, dev);
			if (ret)
				FSLMC_BUS_LOG(ERR, "Unable to probe.\n");
			break;
		}
	}

	return 0;

probe_fail:
	RTE_LOG(DEBUG, EAL, "Error in probing FSLMC Bus. Skipping.\n");
	return 0;
}

/*register a fslmc bus based dpaa2 driver */
void
rte_fslmc_driver_register(struct rte_dpaa2_driver *driver)
{
	RTE_VERIFY(driver);

	TAILQ_INSERT_TAIL(&rte_fslmc_bus.driver_list, driver, next);
	/* Update Bus references */
	driver->fslmc_bus = &rte_fslmc_bus;
}

/*un-register a fslmc bus based dpaa2 driver */
void
rte_fslmc_driver_unregister(struct rte_dpaa2_driver *driver)
{
	struct rte_fslmc_bus *fslmc_bus;

	fslmc_bus = driver->fslmc_bus;

	TAILQ_REMOVE(&fslmc_bus->driver_list, driver, next);
	/* Update Bus references */
	driver->fslmc_bus = NULL;
}

struct rte_fslmc_bus rte_fslmc_bus = {
	.bus = {
		.scan = rte_fslmc_scan,
		.probe = rte_fslmc_probe,
	},
	.device_list = TAILQ_HEAD_INITIALIZER(rte_fslmc_bus.device_list),
	.driver_list = TAILQ_HEAD_INITIALIZER(rte_fslmc_bus.driver_list),
};

RTE_REGISTER_BUS(fslmc, rte_fslmc_bus.bus);
