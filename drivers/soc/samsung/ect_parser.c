#include <soc/samsung/ect_parser.h>

#include <asm/map.h>
#include <asm/memory.h>
#include <asm/uaccess.h>

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#define ALIGNMENT_SIZE 4

#define ECT_PHYS_ADDR 0x95000000
#define ECT_SIZE 0x3A000

#define S5P_VA_ECT (VMALLOC_START + 0xF6000000 + 0x02D00000)

#define ARRAY_SIZE32(array) ((u32)ARRAY_SIZE(array))

/* Variable */

static struct ect_info ect_list[];

static char ect_signature[] = "PARA";

static struct class *ect_class;

static phys_addr_t ect_address;
static phys_addr_t ect_size;

static struct vm_struct ect_early_vm;

/* API for internal */

static void ect_parse_integer(void **address, void *value) {
    *((unsigned int *)value) = __raw_readl(*address);
    *address += sizeof(uint32_t);
}

static void ect_parse_integer64(void **address, void *value) {
    unsigned int top, half;

    half = __raw_readl(*address);
    *address += sizeof(uint32_t);
    top = __raw_readl(*address);
    *address += sizeof(uint32_t);

    *(unsigned long long *)value = ((unsigned long long)top << 32 | half);
}

static int ect_parse_string(void **address, char **value,
                            unsigned int *length) {
    ect_parse_integer(address, length);
    (*length)++;

    *value = *address;

    if (*length % ALIGNMENT_SIZE != 0)
        *address += (unsigned long)(*length + ALIGNMENT_SIZE -
                                    (*length % ALIGNMENT_SIZE));
    else
        *address += (unsigned long)*length;

    return 0;
}

static int ect_parse_dvfs_domain(int parser_version, void *address,
                                 struct ect_dvfs_domain *domain) {
    int ret = 0;
    int i;
    char *clock_name;
    int length;

    ect_parse_integer(&address, &domain->max_frequency);
    ect_parse_integer(&address, &domain->min_frequency);

    if (parser_version >= 2) {
        ect_parse_integer(&address, &domain->boot_level_idx);
        ect_parse_integer(&address, &domain->resume_level_idx);
    } else {
        domain->boot_level_idx = -1;
        domain->resume_level_idx = -1;
    }

    if (parser_version >= 3) {
        ect_parse_integer(&address, &domain->mode);
    } else {
        domain->mode = e_dvfs_mode_clock_name;
    }

    ect_parse_integer(&address, &domain->num_of_clock);
    ect_parse_integer(&address, &domain->num_of_level);

    if (domain->mode == e_dvfs_mode_sfr_address) {
        domain->list_sfr = address;
        domain->list_clock = NULL;

        address += sizeof(unsigned int) * domain->num_of_clock;
    } else if (domain->mode == e_dvfs_mode_clock_name) {
        domain->list_clock =
            kzalloc(sizeof(char *) * domain->num_of_clock, GFP_KERNEL);
        domain->list_sfr = NULL;
        if (domain->list_clock == NULL) {
            ret = -ENOMEM;
            goto err_list_clock_allocation;
        }

        for (i = 0; i < domain->num_of_clock; ++i) {
            if (ect_parse_string(&address, &clock_name, &length)) {
                ret = -EINVAL;
                goto err_parse_string;
            }

            domain->list_clock[i] = clock_name;
        }
    }

    domain->list_level = address;
    address += sizeof(struct ect_dvfs_level) * domain->num_of_level;

    domain->list_dvfs_value = address;

    return 0;

err_parse_string:
    kfree(domain->list_clock);
err_list_clock_allocation:
    return ret;
}

static int ect_parse_dvfs_header(void *address, struct ect_info *info) {
    int ret = 0;
    int i;
    char *domain_name;
    unsigned int length, offset;
    struct ect_dvfs_header *ect_dvfs_header;
    struct ect_dvfs_domain *ect_dvfs_domain;
    void *address_dvfs_header = address;

    if (address == NULL)
        return -EINVAL;

    ect_dvfs_header = kzalloc(sizeof(struct ect_dvfs_header), GFP_KERNEL);
    if (ect_dvfs_header == NULL)
        return -ENOMEM;

    ect_parse_integer(&address, &ect_dvfs_header->parser_version);
    ect_parse_integer(&address, &ect_dvfs_header->version);
    ect_parse_integer(&address, &ect_dvfs_header->num_of_domain);

    ect_dvfs_header->domain_list =
        kzalloc(sizeof(struct ect_dvfs_domain) * ect_dvfs_header->num_of_domain,
                GFP_KERNEL);
    if (ect_dvfs_header->domain_list == NULL) {
        ret = -EINVAL;
        goto err_domain_list_allocation;
    }

    for (i = 0; i < ect_dvfs_header->num_of_domain; ++i) {
        if (ect_parse_string(&address, &domain_name, &length)) {
            ret = -EINVAL;
            goto err_parse_string;
        }

        ect_parse_integer(&address, &offset);

        ect_dvfs_domain = &ect_dvfs_header->domain_list[i];
        ect_dvfs_domain->domain_name = domain_name;
        ect_dvfs_domain->domain_offset = offset;
    }

    for (i = 0; i < ect_dvfs_header->num_of_domain; ++i) {
        ect_dvfs_domain = &ect_dvfs_header->domain_list[i];

        if (ect_parse_dvfs_domain(ect_dvfs_header->parser_version,
                                  address_dvfs_header +
                                      ect_dvfs_domain->domain_offset,
                                  ect_dvfs_domain)) {
            ret = -EINVAL;
            goto err_parse_domain;
        }
    }

    info->block_handle = ect_dvfs_header;

    return 0;

err_parse_domain:
err_parse_string:
    kfree(ect_dvfs_header->domain_list);
err_domain_list_allocation:
    kfree(ect_dvfs_header);
    return ret;
}

static int ect_parse_pll(int parser_version, void *address,
                         struct ect_pll *ect_pll) {
    ect_parse_integer(&address, &ect_pll->type_pll);
    ect_parse_integer(&address, &ect_pll->num_of_frequency);

    ect_pll->frequency_list = address;

    return 0;
}

static int ect_parse_pll_header(void *address, struct ect_info *info) {
    int ret = 0;
    int i;
    char *pll_name;
    unsigned int length, offset;
    struct ect_pll_header *ect_pll_header;
    struct ect_pll *ect_pll;
    void *address_pll_header = address;

    if (address == NULL)
        return -EINVAL;

    ect_pll_header = kzalloc(sizeof(struct ect_pll_header), GFP_KERNEL);
    if (ect_pll_header == NULL)
        return -ENOMEM;

    ect_parse_integer(&address, &ect_pll_header->parser_version);
    ect_parse_integer(&address, &ect_pll_header->version);
    ect_parse_integer(&address, &ect_pll_header->num_of_pll);

    ect_pll_header->pll_list = kzalloc(
        sizeof(struct ect_pll) * ect_pll_header->num_of_pll, GFP_KERNEL);
    if (ect_pll_header->pll_list == NULL) {
        ret = -ENOMEM;
        goto err_pll_list_allocation;
    }

    for (i = 0; i < ect_pll_header->num_of_pll; ++i) {

        if (ect_parse_string(&address, &pll_name, &length)) {
            ret = -EINVAL;
            goto err_parse_string;
        }

        ect_parse_integer(&address, &offset);

        ect_pll = &ect_pll_header->pll_list[i];
        ect_pll->pll_name = pll_name;
        ect_pll->pll_offset = offset;
    }

    for (i = 0; i < ect_pll_header->num_of_pll; ++i) {
        ect_pll = &ect_pll_header->pll_list[i];

        if (ect_parse_pll(ect_pll_header->parser_version,
                          address_pll_header + ect_pll->pll_offset, ect_pll)) {
            ret = -EINVAL;
            goto err_parse_pll;
        }
    }

    info->block_handle = ect_pll_header;

    return 0;

err_parse_pll:
err_parse_string:
    kfree(ect_pll_header->pll_list);
err_pll_list_allocation:
    kfree(ect_pll_header);
    return ret;
}

static int ect_parse_voltage_table(int parser_version, void **address,
                                   struct ect_voltage_domain *domain,
                                   struct ect_voltage_table *table) {
    int num_of_data = domain->num_of_group * domain->num_of_level;

    ect_parse_integer(address, &table->table_version);

    if (parser_version >= 2) {
        ect_parse_integer(address, &table->boot_level_idx);
        ect_parse_integer(address, &table->resume_level_idx);

        table->level_en = *address;
        *address += sizeof(int32_t) * domain->num_of_level;
    } else {
        table->boot_level_idx = -1;
        table->resume_level_idx = -1;

        table->level_en = NULL;
    }

    if (parser_version >= 3) {
        table->voltages = NULL;

        table->voltages_step = *address;
        *address += sizeof(unsigned char) * num_of_data;
        table->volt_step = PMIC_VOLTAGE_STEP;

    } else {
        table->voltages = *address;
        *address += sizeof(int32_t) * num_of_data;

        table->voltages_step = NULL;
        table->volt_step = 0;
    }

    return 0;
}

static int ect_parse_voltage_domain(int parser_version, void *address,
                                    struct ect_voltage_domain *domain) {
    int ret = 0;
    int i;

    ect_parse_integer(&address, &domain->num_of_group);
    ect_parse_integer(&address, &domain->num_of_level);
    ect_parse_integer(&address, &domain->num_of_table);

    domain->level_list = address;
    address += sizeof(int32_t) * domain->num_of_level;

    domain->table_list = kzalloc(
        sizeof(struct ect_voltage_table) * domain->num_of_table, GFP_KERNEL);
    if (domain->table_list == NULL) {
        ret = -ENOMEM;
        goto err_table_list_allocation;
    }

    for (i = 0; i < domain->num_of_table; ++i) {
        if (ect_parse_voltage_table(parser_version, &address, domain,
                                    &domain->table_list[i])) {
            ret = -EINVAL;
            goto err_parse_voltage_table;
        }
    }

    return 0;

err_parse_voltage_table:
    kfree(domain->table_list);
err_table_list_allocation:
    return ret;
}

static int ect_parse_voltage_header(void *address, struct ect_info *info) {
    int ret = 0;
    int i;
    char *domain_name;
    unsigned int length, offset;
    struct ect_voltage_header *ect_voltage_header;
    struct ect_voltage_domain *ect_voltage_domain;
    void *address_voltage_header = address;

    if (address == NULL)
        return -EINVAL;

    ect_voltage_header = kzalloc(sizeof(struct ect_voltage_header), GFP_KERNEL);
    if (ect_voltage_header == NULL)
        return -EINVAL;

    ect_parse_integer(&address, &ect_voltage_header->parser_version);
    ect_parse_integer(&address, &ect_voltage_header->version);
    ect_parse_integer(&address, &ect_voltage_header->num_of_domain);

    ect_voltage_header->domain_list = kzalloc(
        sizeof(struct ect_voltage_domain) * ect_voltage_header->num_of_domain,
        GFP_KERNEL);
    if (ect_voltage_header->domain_list == NULL) {
        ret = -ENOMEM;
        goto err_domain_list_allocation;
    }

    for (i = 0; i < ect_voltage_header->num_of_domain; ++i) {
        if (ect_parse_string(&address, &domain_name, &length)) {
            ret = -EINVAL;
            goto err_parse_string;
        }

        ect_parse_integer(&address, &offset);

        ect_voltage_domain = &ect_voltage_header->domain_list[i];
        ect_voltage_domain->domain_name = domain_name;
        ect_voltage_domain->domain_offset = offset;
    }

    for (i = 0; i < ect_voltage_header->num_of_domain; ++i) {
        ect_voltage_domain = &ect_voltage_header->domain_list[i];

        if (ect_parse_voltage_domain(ect_voltage_header->parser_version,
                                     address_voltage_header +
                                         ect_voltage_domain->domain_offset,
                                     ect_voltage_domain)) {
            ret = -EINVAL;
            goto err_parse_voltage_domain;
        }
    }

    info->block_handle = ect_voltage_header;

    return 0;

err_parse_voltage_domain:
err_parse_string:
    kfree(ect_voltage_header->domain_list);
err_domain_list_allocation:
    kfree(ect_voltage_header);
    return ret;
}

static int ect_parse_rcc_table(int parser_version, void **address,
                               struct ect_rcc_domain *domain,
                               struct ect_rcc_table *table) {
    int num_of_data = domain->num_of_group * domain->num_of_level;

    ect_parse_integer(address, &table->table_version);

    if (parser_version >= 2) {
        table->rcc_compact = *address;
        *address += sizeof(unsigned char) * num_of_data;
    } else {
        table->rcc = *address;
        *address += sizeof(int32_t) * num_of_data;
    }

    return 0;
}

static int ect_parse_rcc_domain(int parser_version, void *address,
                                struct ect_rcc_domain *domain) {
    int ret = 0;
    int i;

    ect_parse_integer(&address, &domain->num_of_group);
    ect_parse_integer(&address, &domain->num_of_level);
    ect_parse_integer(&address, &domain->num_of_table);

    domain->level_list = address;
    address += sizeof(int32_t) * domain->num_of_level;

    domain->table_list = kzalloc(
        sizeof(struct ect_rcc_table) * domain->num_of_table, GFP_KERNEL);
    if (domain->table_list == NULL) {
        ret = -ENOMEM;
        goto err_table_list_allocation;
    }

    for (i = 0; i < domain->num_of_table; ++i) {
        if (ect_parse_rcc_table(parser_version, &address, domain,
                                &domain->table_list[i])) {
            ret = -EINVAL;
            goto err_parse_rcc_table;
        }
    }

    return 0;

err_parse_rcc_table:
    kfree(domain->table_list);
err_table_list_allocation:
    return ret;
}

static int ect_parse_rcc_header(void *address, struct ect_info *info) {
    int ret = 0;
    int i;
    char *domain_name;
    unsigned int length, offset;
    struct ect_rcc_header *ect_rcc_header;
    struct ect_rcc_domain *ect_rcc_domain;
    void *address_rcc_header = address;

    if (address == NULL)
        return -EINVAL;

    ect_rcc_header = kzalloc(sizeof(struct ect_rcc_header), GFP_KERNEL);

    if (ect_rcc_header == NULL)
        return -EINVAL;

    ect_parse_integer(&address, &ect_rcc_header->parser_version);
    ect_parse_integer(&address, &ect_rcc_header->version);
    ect_parse_integer(&address, &ect_rcc_header->num_of_domain);

    ect_rcc_header->domain_list =
        kzalloc(sizeof(struct ect_rcc_domain) * ect_rcc_header->num_of_domain,
                GFP_KERNEL);

    if (ect_rcc_header->domain_list == NULL) {
        ret = -ENOMEM;
        goto err_domain_list_allocation;
    }

    for (i = 0; i < ect_rcc_header->num_of_domain; ++i) {
        if (ect_parse_string(&address, &domain_name, &length)) {
            ret = -EINVAL;
            goto err_parse_string;
        }

        ect_parse_integer(&address, &offset);

        ect_rcc_domain = &ect_rcc_header->domain_list[i];
        ect_rcc_domain->domain_name = domain_name;
        ect_rcc_domain->domain_offset = offset;
    }

    for (i = 0; i < ect_rcc_header->num_of_domain; ++i) {
        ect_rcc_domain = &ect_rcc_header->domain_list[i];

        if (ect_parse_rcc_domain(ect_rcc_header->parser_version,
                                 address_rcc_header +
                                     ect_rcc_domain->domain_offset,
                                 ect_rcc_domain)) {
            ret = -EINVAL;
            goto err_parse_rcc_domain;
        }
    }

    info->block_handle = ect_rcc_header;

    return 0;

err_parse_rcc_domain:
err_parse_string:
    kfree(ect_rcc_header->domain_list);
err_domain_list_allocation:
    kfree(ect_rcc_header);
    return ret;
}

static int ect_parse_mif_thermal_header(void *address, struct ect_info *info) {
    struct ect_mif_thermal_header *ect_mif_thermal_header;

    if (address == NULL)
        return -EINVAL;

    ect_mif_thermal_header =
        kzalloc(sizeof(struct ect_mif_thermal_header), GFP_KERNEL);
    if (ect_mif_thermal_header == NULL)
        return -EINVAL;

    ect_parse_integer(&address, &ect_mif_thermal_header->parser_version);
    ect_parse_integer(&address, &ect_mif_thermal_header->version);
    ect_parse_integer(&address, &ect_mif_thermal_header->num_of_level);

    ect_mif_thermal_header->level = address;

    info->block_handle = ect_mif_thermal_header;

    return 0;
}

static int
ect_parse_ap_thermal_function(int parser_version, void *address,
                              struct ect_ap_thermal_function *function) {
    int i;
    struct ect_ap_thermal_range *range;

    ect_parse_integer(&address, &function->num_of_range);

    function->range_list =
        kzalloc(sizeof(struct ect_ap_thermal_range) * function->num_of_range,
                GFP_KERNEL);

    for (i = 0; i < function->num_of_range; ++i) {
        range = &function->range_list[i];

        ect_parse_integer(&address, &range->lower_bound_temperature);
        ect_parse_integer(&address, &range->upper_bound_temperature);
        ect_parse_integer(&address, &range->max_frequency);
        ect_parse_integer(&address, &range->sw_trip);
        ect_parse_integer(&address, &range->flag);
    }

    return 0;
}

static int ect_parse_ap_thermal_header(void *address, struct ect_info *info) {
    int ret = 0;
    int i;
    char *function_name;
    unsigned int length, offset;
    struct ect_ap_thermal_header *ect_ap_thermal_header;
    struct ect_ap_thermal_function *ect_ap_thermal_function;
    void *address_thermal_header = address;

    if (address == NULL)
        return -EINVAL;

    ect_ap_thermal_header =
        kzalloc(sizeof(struct ect_ap_thermal_header), GFP_KERNEL);
    if (ect_ap_thermal_header == NULL)
        return -EINVAL;

    ect_parse_integer(&address, &ect_ap_thermal_header->parser_version);
    ect_parse_integer(&address, &ect_ap_thermal_header->version);
    ect_parse_integer(&address, &ect_ap_thermal_header->num_of_function);

    ect_ap_thermal_header->function_list =
        kzalloc(sizeof(struct ect_ap_thermal_function) *
                    ect_ap_thermal_header->num_of_function,
                GFP_KERNEL);
    if (ect_ap_thermal_header->function_list == NULL) {
        ret = -ENOMEM;
        goto err_function_list_allocation;
    }

    for (i = 0; i < ect_ap_thermal_header->num_of_function; ++i) {
        if (ect_parse_string(&address, &function_name, &length)) {
            ret = -EINVAL;
            goto err_parse_string;
        }

        ect_parse_integer(&address, &offset);

        ect_ap_thermal_function = &ect_ap_thermal_header->function_list[i];
        ect_ap_thermal_function->function_name = function_name;
        ect_ap_thermal_function->function_offset = offset;
    }

    for (i = 0; i < ect_ap_thermal_header->num_of_function; ++i) {
        ect_ap_thermal_function = &ect_ap_thermal_header->function_list[i];

        if (ect_parse_ap_thermal_function(
                ect_ap_thermal_header->parser_version,
                address_thermal_header +
                    ect_ap_thermal_function->function_offset,
                ect_ap_thermal_function)) {
            ret = -EINVAL;
            goto err_parse_ap_thermal_function;
        }
    }

    info->block_handle = ect_ap_thermal_header;

    return 0;

err_parse_ap_thermal_function:
err_parse_string:
    kfree(ect_ap_thermal_header->function_list);
err_function_list_allocation:
    kfree(ect_ap_thermal_header);
    return ret;
}

static int ect_parse_margin_domain(int parser_version, void *address,
                                   struct ect_margin_domain *domain) {
    ect_parse_integer(&address, &domain->num_of_group);
    ect_parse_integer(&address, &domain->num_of_level);

    if (parser_version >= 2) {
        domain->offset = NULL;
        domain->offset_compact = address;
        domain->volt_step = PMIC_VOLTAGE_STEP;
    } else {
        domain->offset = address;
        domain->offset_compact = NULL;
    }

    return 0;
}

static int ect_parse_margin_header(void *address, struct ect_info *info) {
    int ret = 0;
    int i;
    char *domain_name;
    unsigned int length, offset;
    struct ect_margin_header *ect_margin_header;
    struct ect_margin_domain *ect_margin_domain;
    void *address_margin_header = address;

    if (address == NULL)
        return -EINVAL;

    ect_margin_header = kzalloc(sizeof(struct ect_margin_header), GFP_KERNEL);
    if (ect_margin_header == NULL)
        return -EINVAL;

    ect_parse_integer(&address, &ect_margin_header->parser_version);
    ect_parse_integer(&address, &ect_margin_header->version);
    ect_parse_integer(&address, &ect_margin_header->num_of_domain);

    ect_margin_header->domain_list = kzalloc(
        sizeof(struct ect_margin_domain) * ect_margin_header->num_of_domain,
        GFP_KERNEL);
    if (ect_margin_header->domain_list == NULL) {
        ret = -ENOMEM;
        goto err_domain_list_allocation;
    }

    for (i = 0; i < ect_margin_header->num_of_domain; ++i) {
        if (ect_parse_string(&address, &domain_name, &length)) {
            ret = -EINVAL;
            goto err_parse_string;
        }

        ect_parse_integer(&address, &offset);

        ect_margin_domain = &ect_margin_header->domain_list[i];
        ect_margin_domain->domain_name = domain_name;
        ect_margin_domain->domain_offset = offset;
    }

    for (i = 0; i < ect_margin_header->num_of_domain; ++i) {
        ect_margin_domain = &ect_margin_header->domain_list[i];

        if (ect_parse_margin_domain(ect_margin_header->parser_version,
                                    address_margin_header +
                                        ect_margin_domain->domain_offset,
                                    ect_margin_domain)) {
            ret = -EINVAL;
            goto err_parse_margin_domain;
        }
    }

    info->block_handle = ect_margin_header;

    return 0;

err_parse_margin_domain:
err_parse_string:
    kfree(ect_margin_header->domain_list);
err_domain_list_allocation:
    kfree(ect_margin_header);
    return ret;
}

static int ect_parse_timing_param_size(int parser_version, void *address,
                                       struct ect_timing_param_size *size) {
    ect_parse_integer(&address, &size->num_of_timing_param);
    ect_parse_integer(&address, &size->num_of_level);

    size->timing_parameter = address;

    return 0;
}

static int ect_parse_timing_param_header(void *address, struct ect_info *info) {
    int ret = 0;
    int i;
    struct ect_timing_param_header *ect_timing_param_header;
    struct ect_timing_param_size *ect_timing_param_size;
    void *address_param_header = address;

    if (address == NULL)
        return -EINVAL;

    ect_timing_param_header =
        kzalloc(sizeof(struct ect_timing_param_header), GFP_KERNEL);
    if (ect_timing_param_header == NULL)
        return -ENOMEM;

    ect_parse_integer(&address, &ect_timing_param_header->parser_version);
    ect_parse_integer(&address, &ect_timing_param_header->version);
    ect_parse_integer(&address, &ect_timing_param_header->num_of_size);

    ect_timing_param_header->size_list =
        kzalloc(sizeof(struct ect_timing_param_size) *
                    ect_timing_param_header->num_of_size,
                GFP_KERNEL);
    if (ect_timing_param_header->size_list == NULL) {
        ret = -ENOMEM;
        goto err_size_list_allocation;
    }

    for (i = 0; i < ect_timing_param_header->num_of_size; ++i) {
        ect_timing_param_size = &ect_timing_param_header->size_list[i];

        if (ect_timing_param_header->parser_version >= 3) {
            ect_parse_integer64(&address,
                                &ect_timing_param_size->parameter_key);
            ect_timing_param_size->memory_size =
                (unsigned int)ect_timing_param_size->parameter_key;
        } else {
            ect_parse_integer(&address, &ect_timing_param_size->memory_size);
            ect_timing_param_size->parameter_key =
                ect_timing_param_size->memory_size;
        }

        ect_parse_integer(&address, &ect_timing_param_size->offset);
    }

    for (i = 0; i < ect_timing_param_header->num_of_size; ++i) {
        ect_timing_param_size = &ect_timing_param_header->size_list[i];

        if (ect_parse_timing_param_size(ect_timing_param_header->parser_version,
                                        address_param_header +
                                            ect_timing_param_size->offset,
                                        ect_timing_param_size)) {
            ret = -EINVAL;
            goto err_parse_timing_param_size;
        }
    }

    info->block_handle = ect_timing_param_header;

    return 0;

err_parse_timing_param_size:
    kfree(ect_timing_param_header->size_list);
err_size_list_allocation:
    kfree(ect_timing_param_header);
    return ret;
}

static int ect_parse_minlock_domain(int parser_version, void *address,
                                    struct ect_minlock_domain *domain) {
    ect_parse_integer(&address, &domain->num_of_level);

    domain->level = address;

    return 0;
}

static int ect_parse_minlock_header(void *address, struct ect_info *info) {
    int ret = 0;
    int i;
    char *domain_name;
    unsigned int length, offset;
    struct ect_minlock_header *ect_minlock_header;
    struct ect_minlock_domain *ect_minlock_domain;
    void *address_minlock_header = address;

    if (address == NULL)
        return -EINVAL;

    ect_minlock_header = kzalloc(sizeof(struct ect_minlock_header), GFP_KERNEL);
    if (ect_minlock_header == NULL)
        return -ENOMEM;

    ect_parse_integer(&address, &ect_minlock_header->parser_version);
    ect_parse_integer(&address, &ect_minlock_header->version);
    ect_parse_integer(&address, &ect_minlock_header->num_of_domain);

    ect_minlock_header->domain_list = kzalloc(
        sizeof(struct ect_minlock_domain) * ect_minlock_header->num_of_domain,
        GFP_KERNEL);
    if (ect_minlock_header->domain_list == NULL) {
        ret = -ENOMEM;
        goto err_domain_list_allocation;
    }

    for (i = 0; i < ect_minlock_header->num_of_domain; ++i) {
        if (ect_parse_string(&address, &domain_name, &length)) {
            ret = -EINVAL;
            goto err_parse_string;
        }

        ect_parse_integer(&address, &offset);

        ect_minlock_domain = &ect_minlock_header->domain_list[i];
        ect_minlock_domain->domain_name = domain_name;
        ect_minlock_domain->domain_offset = offset;
    }

    for (i = 0; i < ect_minlock_header->num_of_domain; ++i) {
        ect_minlock_domain = &ect_minlock_header->domain_list[i];

        if (ect_parse_minlock_domain(ect_minlock_header->parser_version,
                                     address_minlock_header +
                                         ect_minlock_domain->domain_offset,
                                     ect_minlock_domain)) {
            ret = -EINVAL;
            goto err_parse_minlock_domain;
        }
    }

    info->block_handle = ect_minlock_header;

    return 0;

err_parse_minlock_domain:
err_parse_string:
    kfree(ect_minlock_header->domain_list);
err_domain_list_allocation:
    kfree(ect_minlock_header);
    return ret;
}

static int ect_parse_gen_param_table(int parser_version, void *address,
                                     struct ect_gen_param_table *size) {
    ect_parse_integer(&address, &size->num_of_col);
    ect_parse_integer(&address, &size->num_of_row);

    size->parameter = address;

    return 0;
}

static int ect_parse_gen_param_header(void *address, struct ect_info *info) {
    int ret = 0;
    int i;
    char *table_name;
    unsigned int length, offset;
    struct ect_gen_param_header *ect_gen_param_header;
    struct ect_gen_param_table *ect_gen_param_table;
    void *address_param_header = address;

    if (address == NULL)
        return -EINVAL;

    ect_gen_param_header =
        kzalloc(sizeof(struct ect_gen_param_header), GFP_KERNEL);
    if (ect_gen_param_header == NULL)
        return -ENOMEM;

    ect_parse_integer(&address, &ect_gen_param_header->parser_version);
    ect_parse_integer(&address, &ect_gen_param_header->version);
    ect_parse_integer(&address, &ect_gen_param_header->num_of_table);

    ect_gen_param_header->table_list = kzalloc(
        sizeof(struct ect_gen_param_table) * ect_gen_param_header->num_of_table,
        GFP_KERNEL);
    if (ect_gen_param_header->table_list == NULL) {
        ret = -ENOMEM;
        goto err_table_list_allocation;
    }

    for (i = 0; i < ect_gen_param_header->num_of_table; ++i) {
        if (ect_parse_string(&address, &table_name, &length)) {
            ret = -EINVAL;
            goto err_parse_string;
        }

        ect_parse_integer(&address, &offset);

        ect_gen_param_table = &ect_gen_param_header->table_list[i];
        ect_gen_param_table->table_name = table_name;
        ect_gen_param_table->offset = offset;
    }

    for (i = 0; i < ect_gen_param_header->num_of_table; ++i) {
        ect_gen_param_table = &ect_gen_param_header->table_list[i];

        if (ect_parse_gen_param_table(ect_gen_param_header->parser_version,
                                      address_param_header +
                                          ect_gen_param_table->offset,
                                      ect_gen_param_table)) {
            ret = -EINVAL;
            goto err_parse_gen_param_table;
        }
    }

    info->block_handle = ect_gen_param_header;

    return 0;

err_parse_gen_param_table:
err_parse_string:
    kfree(ect_gen_param_header->table_list);
err_table_list_allocation:
    kfree(ect_gen_param_header);
    return ret;
}

static int ect_parse_bin(int parser_version, void *address,
                         struct ect_bin *binary) {
    ect_parse_integer(&address, &binary->binary_size);

    binary->ptr = address;

    return 0;
}

static int ect_parse_bin_header(void *address, struct ect_info *info) {
    int ret = 0;
    int i;
    struct ect_bin_header *ect_bin_header;
    struct ect_bin *ect_binary_bin;
    void *address_param_header = address;
    char *binary_name;
    int offset, length;

    if (address == NULL)
        return -1;

    ect_bin_header = kzalloc(sizeof(struct ect_bin_header), GFP_KERNEL);
    if (ect_bin_header == NULL)
        return -2;

    ect_parse_integer(&address, &ect_bin_header->parser_version);
    ect_parse_integer(&address, &ect_bin_header->version);
    ect_parse_integer(&address, &ect_bin_header->num_of_binary);

    ect_bin_header->binary_list = kzalloc(
        sizeof(struct ect_bin) * ect_bin_header->num_of_binary, GFP_KERNEL);
    if (ect_bin_header->binary_list == NULL) {
        ret = -ENOMEM;
        goto err_binary_list_allocation;
    }

    for (i = 0; i < ect_bin_header->num_of_binary; ++i) {
        ect_binary_bin = &ect_bin_header->binary_list[i];

        if (ect_parse_string(&address, &binary_name, &length)) {
            ret = -EINVAL;
            goto err_parse_string;
        }

        ect_parse_integer(&address, &offset);
        ect_binary_bin->binary_name = binary_name;
        ect_binary_bin->offset = offset;
    }

    for (i = 0; i < ect_bin_header->num_of_binary; ++i) {
        ect_binary_bin = &ect_bin_header->binary_list[i];

        if (ect_parse_bin(ect_bin_header->parser_version,
                          address_param_header + ect_binary_bin->offset,
                          ect_binary_bin)) {
            ret = -EINVAL;
            goto err_parse_bin;
        }
    }

    info->block_handle = ect_bin_header;

    return 0;

err_parse_bin:
err_parse_string:
    kfree(ect_bin_header->binary_list);
err_binary_list_allocation:
    kfree(ect_bin_header);
    return ret;
}

static int
ect_parse_new_timing_param_size(int parser_version, void *address,
                                struct ect_new_timing_param_size *size) {
    ect_parse_integer(&address, &size->mode);
    ect_parse_integer(&address, &size->num_of_timing_param);
    ect_parse_integer(&address, &size->num_of_level);

    size->timing_parameter = address;

    return 0;
}

static int ect_parse_new_timing_param_header(void *address,
                                             struct ect_info *info) {
    int ret = 0;
    int i;
    struct ect_new_timing_param_header *ect_new_timing_param_header;
    struct ect_new_timing_param_size *ect_new_timing_param_size;
    void *address_param_header = address;

    if (address == NULL)
        return -EINVAL;

    ect_new_timing_param_header =
        kzalloc(sizeof(struct ect_new_timing_param_header), GFP_KERNEL);
    if (ect_new_timing_param_header == NULL)
        return -ENOMEM;

    ect_parse_integer(&address, &ect_new_timing_param_header->parser_version);
    ect_parse_integer(&address, &ect_new_timing_param_header->version);
    ect_parse_integer(&address, &ect_new_timing_param_header->num_of_size);

    ect_new_timing_param_header->size_list =
        kzalloc(sizeof(struct ect_new_timing_param_size) *
                    ect_new_timing_param_header->num_of_size,
                GFP_KERNEL);
    if (ect_new_timing_param_header->size_list == NULL) {
        ret = -ENOMEM;
        goto err_size_list_allocation;
    }

    for (i = 0; i < ect_new_timing_param_header->num_of_size; ++i) {
        ect_new_timing_param_size = &ect_new_timing_param_header->size_list[i];

        ect_parse_integer64(&address,
                            &ect_new_timing_param_size->parameter_key);
        ect_parse_integer(&address, &ect_new_timing_param_size->offset);
    }

    for (i = 0; i < ect_new_timing_param_header->num_of_size; ++i) {
        ect_new_timing_param_size = &ect_new_timing_param_header->size_list[i];

        if (ect_parse_new_timing_param_size(
                ect_new_timing_param_header->parser_version,
                address_param_header + ect_new_timing_param_size->offset,
                ect_new_timing_param_size)) {
            ret = -EINVAL;
            goto err_parse_new_timing_param_size;
        }
    }

    info->block_handle = ect_new_timing_param_header;

    return 0;

err_parse_new_timing_param_size:
    kfree(ect_new_timing_param_header->size_list);
err_size_list_allocation:
    kfree(ect_new_timing_param_header);
    return ret;
}

static int ect_parse_pidtm_block(int parser_version, void *address,
                                 struct ect_pidtm_block *block) {
    int ret = 0;
    int i, length;

    ect_parse_integer(&address, &block->num_of_temperature);
    block->temperature_list = address;

    address += sizeof(int32_t) * block->num_of_temperature;

    ect_parse_integer(&address, &block->num_of_parameter);
    block->param_name_list =
        kzalloc(sizeof(char *) * block->num_of_parameter, GFP_KERNEL);
    if (block->param_name_list == NULL) {
        ret = -ENOMEM;
        goto err_param_name_list_allocation;
    }

    for (i = 0; i < block->num_of_parameter; ++i) {
        if (ect_parse_string(&address, &block->param_name_list[i], &length)) {
            ret = -EINVAL;
            goto err_parse_param_name;
        }
    }

    block->param_value_list = address;

    return 0;

err_parse_param_name:
    kfree(block->param_name_list);
err_param_name_list_allocation:
    return ret;
}

static int ect_parse_pidtm_header(void *address, struct ect_info *info) {
    int ret = 0;
    int i;
    struct ect_pidtm_header *ect_pidtm_header;
    struct ect_pidtm_block *ect_pidtm_block;
    void *address_param_header = address;
    char *block_name;
    int offset, length;

    if (address == NULL)
        return -EINVAL;

    ect_pidtm_header = kzalloc(sizeof(struct ect_pidtm_header), GFP_KERNEL);
    if (ect_pidtm_header == NULL)
        return -ENOMEM;

    ect_parse_integer(&address, &ect_pidtm_header->parser_version);
    ect_parse_integer(&address, &ect_pidtm_header->version);
    ect_parse_integer(&address, &ect_pidtm_header->num_of_block);

    ect_pidtm_header->block_list =
        kzalloc(sizeof(struct ect_pidtm_block) * ect_pidtm_header->num_of_block,
                GFP_KERNEL);
    if (ect_pidtm_header->block_list == NULL) {
        ret = -ENOMEM;
        goto err_block_list_allocation;
    }

    for (i = 0; i < ect_pidtm_header->num_of_block; ++i) {
        ect_pidtm_block = &ect_pidtm_header->block_list[i];

        if (ect_parse_string(&address, &block_name, &length)) {
            ret = -EINVAL;
            goto err_parse_string;
        }

        ect_parse_integer(&address, &offset);
        ect_pidtm_block->block_name = block_name;
        ect_pidtm_block->offset = offset;
    }

    for (i = 0; i < ect_pidtm_header->num_of_block; ++i) {
        ect_pidtm_block = &ect_pidtm_header->block_list[i];

        if (ect_parse_pidtm_block(ect_pidtm_header->parser_version,
                                  address_param_header +
                                      ect_pidtm_block->offset,
                                  ect_pidtm_block)) {
            ret = -EINVAL;
            goto err_parse_pidtm_block;
        }
    }

    info->block_handle = ect_pidtm_header;

    return 0;

err_parse_pidtm_block:
err_parse_string:
    kfree(ect_pidtm_header->block_list);
err_block_list_allocation:
    kfree(ect_pidtm_header);
    return ret;
}

static void ect_present_test_data(char *version) {
    if (version[1] == '.')
        return;

    if (version[3] == '0')
        return;

    pr_info("========================================\n");
    pr_info("=\n");
    pr_info("= [ECT] current version is TEST VERSION!!\n");
    pr_info("= Please be aware that error can be happen.\n");
    pr_info("= [VERSION] : %c%c%c%c\n", version[0], version[1], version[2],
            version[3]);
    pr_info("=\n");
    pr_info("========================================\n");
}

#if defined(CONFIG_ECT_DUMP)

static int ect_dump_header(struct seq_file *s, void *data);
static int ect_dump_dvfs(struct seq_file *s, void *data);
static int ect_dump_pll(struct seq_file *s, void *data);
static int ect_dump_voltage(struct seq_file *s, void *data);
static int ect_dump_rcc(struct seq_file *s, void *data);
static int ect_dump_mif_thermal(struct seq_file *s, void *data);
static int ect_dump_ap_thermal(struct seq_file *s, void *data);
static int ect_dump_margin(struct seq_file *s, void *data);
static int ect_dump_timing_parameter(struct seq_file *s, void *data);
static int ect_dump_minlock(struct seq_file *s, void *data);
static int ect_dump_gen_parameter(struct seq_file *s, void *data);
static int ect_dump_binary(struct seq_file *s, void *data);
static int ect_dump_new_timing_parameter(struct seq_file *s, void *data);
static int ect_dump_pidtm(struct seq_file *s, void *data);

static int dump_open(struct inode *inode, struct file *file);

#else

#define ect_dump_header NULL
#define ect_dump_ap_thermal NULL
#define ect_dump_voltage NULL
#define ect_dump_dvfs NULL
#define ect_dump_margin NULL
#define ect_dump_mif_thermal NULL
#define ect_dump_pll NULL
#define ect_dump_rcc NULL
#define ect_dump_timing_parameter NULL
#define ect_dump_minlock NULL
#define ect_dump_gen_parameter NULL
#define ect_dump_binary NULL
#define ect_dump_new_timing_parameter NULL
#define ect_dump_pidtm NULL

#define dump_open NULL

#endif

static struct ect_info ect_header_info = {
    .block_name = BLOCK_HEADER,
    .dump = ect_dump_header,
    .dump_ops =
        {
            .open = dump_open,
            .read = seq_read,
            .llseek = seq_lseek,
            .release = single_release,
        },
    .dump_node_name = SYSFS_NODE_HEADER,
    .block_handle = NULL,
    .block_precedence = -1,
};

static struct ect_info ect_list[] =
    {{
         .block_name = BLOCK_AP_THERMAL,
         .block_name_length = sizeof(BLOCK_AP_THERMAL) - 1,
         .parser = ect_parse_ap_thermal_header,
         .dump = ect_dump_ap_thermal,
         .dump_ops = {.open = dump_open,
                      .read = seq_read,
                      .llseek = seq_lseek,
                      .release = single_release},
         .dump_node_name = SYSFS_NODE_AP_THERMAL,
         .block_handle = NULL,
         .block_precedence = -1,
     },
     {
         .block_name = BLOCK_ASV,
         .block_name_length = sizeof(BLOCK_ASV) - 1,
         .parser = ect_parse_voltage_header,
         .dump = ect_dump_voltage,
         .dump_ops = {.open = dump_open,
                      .read = seq_read,
                      .llseek = seq_lseek,
                      .release = single_release},
         .dump_node_name = SYSFS_NODE_ASV,
         .block_handle = NULL,
         .block_precedence = -1,
     },
     {
         .block_name = BLOCK_DVFS,
         .block_name_length = sizeof(BLOCK_DVFS) - 1,
         .parser = ect_parse_dvfs_header,
         .dump = ect_dump_dvfs,
         .dump_ops = {.open = dump_open,
                      .read = seq_read,
                      .llseek = seq_lseek,
                      .release = single_release},
         .dump_node_name = SYSFS_NODE_DVFS,
         .block_handle = NULL,
         .block_precedence = -1,
     },
     {
         .block_name = BLOCK_MARGIN,
         .block_name_length = sizeof(BLOCK_MARGIN) - 1,
         .parser = ect_parse_margin_header,
         .dump = ect_dump_margin,
         .dump_ops = {.open = dump_open,
                      .read = seq_read,
                      .llseek = seq_lseek,
                      .release = single_release},
         .dump_node_name = SYSFS_NODE_MARGIN,
         .block_handle = NULL,
         .block_precedence = -1,
     },
     {
         .block_name = BLOCK_MIF_THERMAL,
         .block_name_length = sizeof(BLOCK_MIF_THERMAL) - 1,
         .parser = ect_parse_mif_thermal_header,
         .dump = ect_dump_mif_thermal,
         .dump_ops = {.open = dump_open,
                      .read = seq_read,
                      .llseek = seq_lseek,
                      .release = single_release},
         .dump_node_name = SYSFS_NODE_MIF_THERMAL,
         .block_handle = NULL,
         .block_precedence = -1,
     },
     {
         .block_name = BLOCK_PLL,
         .block_name_length = sizeof(BLOCK_PLL) - 1,
         .parser = ect_parse_pll_header,
         .dump = ect_dump_pll,
         .dump_ops = {.open = dump_open,
                      .read = seq_read,
                      .llseek = seq_lseek,
                      .release = single_release},
         .dump_node_name = SYSFS_NODE_PLL,
         .block_handle = NULL,
         .block_precedence = -1,
     },
     {
         .block_name = BLOCK_RCC,
         .block_name_length = sizeof(BLOCK_RCC) - 1,
         .parser = ect_parse_rcc_header,
         .dump = ect_dump_rcc,
         .dump_ops = {.open = dump_open,
                      .read = seq_read,
                      .llseek = seq_lseek,
                      .release = single_release},
         .dump_node_name = SYSFS_NODE_RCC,
         .block_handle = NULL,
         .block_precedence = -1,
     },
     {
         .block_name = BLOCK_TIMING_PARAM,
         .block_name_length = sizeof(BLOCK_TIMING_PARAM) - 1,
         .parser = ect_parse_timing_param_header,
         .dump = ect_dump_timing_parameter,
         .dump_ops = {.open = dump_open,
                      .read = seq_read,
                      .llseek = seq_lseek,
                      .release = single_release},
         .dump_node_name = SYSFS_NODE_TIMING_PARAM,
         .block_handle = NULL,
         .block_precedence = -1,
     },
     {
         .block_name = BLOCK_MINLOCK,
         .block_name_length = sizeof(BLOCK_MINLOCK) - 1,
         .parser = ect_parse_minlock_header,
         .dump = ect_dump_minlock,
         .dump_ops = {.open = dump_open,
                      .read = seq_read,
                      .llseek = seq_lseek,
                      .release = single_release},
         .dump_node_name = SYSFS_NODE_MINLOCK,
         .block_handle = NULL,
         .block_precedence = -1,
     },
     {
         .block_name = BLOCK_GEN_PARAM,
         .block_name_length = sizeof(BLOCK_GEN_PARAM) - 1,
         .parser = ect_parse_gen_param_header,
         .dump = ect_dump_gen_parameter,
         .dump_ops = {.open = dump_open,
                      .read = seq_read,
                      .llseek = seq_lseek,
                      .release = single_release},
         .dump_node_name = SYSFS_NODE_GEN_PARAM,
         .block_handle = NULL,
         .block_precedence = -1,
     },
     {
         .block_name = BLOCK_BIN,
         .block_name_length = sizeof(BLOCK_BIN) - 1,
         .parser = ect_parse_bin_header,
         .dump = ect_dump_binary,
         .dump_ops = {.open = dump_open,
                      .read = seq_read,
                      .llseek = seq_lseek,
                      .release = single_release},
         .dump_node_name = SYSFS_NODE_BIN,
         .block_handle = NULL,
         .block_precedence = -1,
     },
     {
         .block_name = BLOCK_NEW_TIMING_PARAM,
         .block_name_length = sizeof(BLOCK_NEW_TIMING_PARAM) - 1,
         .parser = ect_parse_new_timing_param_header,
         .dump = ect_dump_new_timing_parameter,
         .dump_ops =
             {
                 .open = dump_open,
                 .read = seq_read,
                 .llseek = seq_lseek,
                 .release = single_release,
             },
         .dump_node_name = SYSFS_NODE_NEW_TIMING_PARAM,
         .block_handle = NULL,
         .block_precedence = -1,
     },
     {
         .block_name = BLOCK_PIDTM,
         .block_name_length = sizeof(BLOCK_PIDTM) - 1,
         .parser = ect_parse_pidtm_header,
         .dump = ect_dump_pidtm,
         .dump_ops =
             {
                 .open = dump_open,
                 .read = seq_read,
                 .llseek = seq_lseek,
                 .release = single_release,
             },
         .dump_node_name = SYSFS_NODE_PIDTM,
         .block_handle = NULL,
         .block_precedence = -1,
     }};

#if defined(CONFIG_ECT_DUMP)

static struct ect_info *ect_get_info(char *block_name) {
    int i;

    for (i = 0; i < ARRAY_SIZE32(ect_list); ++i) {
        if (ect_strcmp(block_name, ect_list[i].block_name) == 0)
            return &ect_list[i];
    }

    return NULL;
}

static int ect_dump_header(struct seq_file *s, void *data) {
    struct ect_info *info = &ect_header_info;
    struct ect_header *header = info->block_handle;

    if (header == NULL) {
        seq_printf(s, "[ECT] : there is no ECT Information\n");
        return 0;
    }

    seq_printf(s, "[ECT] : ECT Information\n");
    seq_printf(s, "\t[VA] : %p\n", (void *)S5P_VA_ECT);
    seq_printf(s, "\t[SIGN] : %c%c%c%c\n", header->sign[0], header->sign[1],
               header->sign[2], header->sign[3]);
    seq_printf(s, "\t[VERSION] : %c%c%c%c\n", header->version[0],
               header->version[1], header->version[2], header->version[3]);
    seq_printf(s, "\t[TOTAL SIZE] : %d\n", header->total_size);
    seq_printf(s, "\t[NUM OF HEADER] : %d\n", header->num_of_header);

    return 0;
}

static int ect_dump_dvfs(struct seq_file *s, void *data) {
    int i, j, k;
    struct ect_info *info = ect_get_info(BLOCK_DVFS);
    struct ect_dvfs_header *ect_dvfs_header = info->block_handle;
    struct ect_dvfs_domain *domain;

    if (ect_dvfs_header == NULL) {
        seq_printf(s, "[ECT] : there is no dvfs information\n");
        return 0;
    }

    seq_printf(s, "[ECT] : DVFS Information\n");
    seq_printf(s, "\t[PARSER VERSION] : %d\n", ect_dvfs_header->parser_version);
    seq_printf(s, "\t[VERSION] : %c%c%c%c\n", ect_dvfs_header->version[0],
               ect_dvfs_header->version[1], ect_dvfs_header->version[2],
               ect_dvfs_header->version[3]);
    seq_printf(s, "\t[NUM OF DOMAIN] : %d\n", ect_dvfs_header->num_of_domain);

    for (i = 0; i < ect_dvfs_header->num_of_domain; ++i) {
        domain = &ect_dvfs_header->domain_list[i];

        seq_printf(s, "\t\t[DOMAIN NAME] : %s\n", domain->domain_name);
        seq_printf(s, "\t\t[BOOT LEVEL IDX] : ");
        if (domain->boot_level_idx == -1) {
            seq_printf(s, "NONE\n");
        } else {
            seq_printf(s, "%d\n", domain->boot_level_idx);
        }
        seq_printf(s, "\t\t[RESUME LEVEL IDX] : ");
        if (domain->resume_level_idx == -1) {
            seq_printf(s, "NONE\n");
        } else {
            seq_printf(s, "%d\n", domain->resume_level_idx);
        }
        seq_printf(s, "\t\t[MAX FREQ] : %u\n", domain->max_frequency);
        seq_printf(s, "\t\t[MIN FREQ] : %u\n", domain->min_frequency);
        if (domain->mode == e_dvfs_mode_clock_name) {
            seq_printf(s, "\t\t[NUM OF CLOCK] : %d\n", domain->num_of_clock);

            for (j = 0; j < domain->num_of_clock; ++j) {
                seq_printf(s, "\t\t\t[CLOCK NAME] : %s\n",
                           domain->list_clock[j]);
            }
        } else if (domain->mode == e_dvfs_mode_sfr_address) {
            seq_printf(s, "\t\t[NUM OF SFR] : %d\n", domain->num_of_clock);

            for (j = 0; j < domain->num_of_clock; ++j) {
                seq_printf(s, "\t\t\t[SFR ADDRESS] : %x\n",
                           domain->list_sfr[j]);
            }
        }

        seq_printf(s, "\t\t[NUM OF LEVEL] : %d\n", domain->num_of_level);

        for (j = 0; j < domain->num_of_level; ++j) {
            seq_printf(s, "\t\t\t[LEVEL] : %u(%c)\n",
                       domain->list_level[j].level,
                       domain->list_level[j].level_en ? 'O' : 'X');
        }

        seq_printf(s, "\t\t\t\t[TABLE]\n");
        for (j = 0; j < domain->num_of_level; ++j) {
            seq_printf(s, "\t\t\t\t");
            for (k = 0; k < domain->num_of_clock; ++k) {
                seq_printf(
                    s, "%u ",
                    domain->list_dvfs_value[j * domain->num_of_clock + k]);
            }
            seq_printf(s, "\n");
        }
    }

    return 0;
}

static int ect_dump_pll(struct seq_file *s, void *data) {
    int i, j;
    struct ect_info *info = ect_get_info(BLOCK_PLL);
    struct ect_pll_header *ect_pll_header = info->block_handle;
    struct ect_pll *pll;
    struct ect_pll_frequency *frequency;

    if (ect_pll_header == NULL) {
        seq_printf(s, "[ECT] : there is no pll information\n");
        return 0;
    }

    seq_printf(s, "[ECT] : PLL Information\n");
    seq_printf(s, "\t[PARSER VERSION] : %d\n", ect_pll_header->parser_version);
    seq_printf(s, "\t[VERSION] : %c%c%c%c\n", ect_pll_header->version[0],
               ect_pll_header->version[1], ect_pll_header->version[2],
               ect_pll_header->version[3]);
    seq_printf(s, "\t[NUM OF PLL] : %d\n", ect_pll_header->num_of_pll);

    for (i = 0; i < ect_pll_header->num_of_pll; ++i) {
        pll = &ect_pll_header->pll_list[i];

        seq_printf(s, "\t\t[PLL NAME] : %s\n", pll->pll_name);
        seq_printf(s, "\t\t[PLL TYPE] : %d\n", pll->type_pll);
        seq_printf(s, "\t\t[NUM OF FREQUENCY] : %d\n", pll->num_of_frequency);

        for (j = 0; j < pll->num_of_frequency; ++j) {
            frequency = &pll->frequency_list[j];

            seq_printf(s, "\t\t\t[FREQUENCY] : %u\n", frequency->frequency);
            seq_printf(s, "\t\t\t[P] : %d\n", frequency->p);
            seq_printf(s, "\t\t\t[M] : %d\n", frequency->m);
            seq_printf(s, "\t\t\t[S] : %d\n", frequency->s);
            seq_printf(s, "\t\t\t[K] : %d\n", frequency->k);
        }
    }

    return 0;
}

static int ect_dump_voltage(struct seq_file *s, void *data) {
    int i, j, k, l;
    struct ect_info *info = ect_get_info(BLOCK_ASV);
    struct ect_voltage_header *ect_voltage_header = info->block_handle;
    struct ect_voltage_domain *domain;

    if (ect_voltage_header == NULL) {
        seq_printf(s, "[ECT] : there is no asv information\n");
        return 0;
    }

    seq_printf(s, "[ECT] : ASV Voltage Information\n");
    seq_printf(s, "\t[PARSER VERSION] : %d\n",
               ect_voltage_header->parser_version);
    seq_printf(s, "\t[VERSION] : %c%c%c%c\n", ect_voltage_header->version[0],
               ect_voltage_header->version[1], ect_voltage_header->version[2],
               ect_voltage_header->version[3]);
    seq_printf(s, "\t[NUM OF DOMAIN] : %d\n",
               ect_voltage_header->num_of_domain);

    for (i = 0; i < ect_voltage_header->num_of_domain; ++i) {
        domain = &ect_voltage_header->domain_list[i];

        seq_printf(s, "\t\t[DOMAIN NAME] : %s\n", domain->domain_name);
        seq_printf(s, "\t\t[NUM OF ASV GROUP] : %d\n", domain->num_of_group);
        seq_printf(s, "\t\t[NUM OF LEVEL] : %d\n", domain->num_of_level);

        for (j = 0; j < domain->num_of_level; ++j) {
            seq_printf(s, "\t\t\t[FREQUENCY] : %u\n", domain->level_list[j]);
        }

        seq_printf(s, "\t\t[NUM OF TABLE] : %d\n", domain->num_of_table);

        for (j = 0; j < domain->num_of_table; ++j) {
            seq_printf(s, "\t\t\t[TABLE VERSION] : %d\n",
                       domain->table_list[j].table_version);
            seq_printf(s, "\t\t\t[BOOT LEVEL IDX] : ");
            if (domain->table_list[j].boot_level_idx == -1) {
                seq_printf(s, "NONE\n");
            } else {
                seq_printf(s, "%d\n", domain->table_list[j].boot_level_idx);
            }
            seq_printf(s, "\t\t\t[RESUME LEVEL IDX] : ");
            if (domain->table_list[j].resume_level_idx == -1) {
                seq_printf(s, "NONE\n");
            } else {
                seq_printf(s, "%d\n", domain->table_list[j].resume_level_idx);
            }
            seq_printf(s, "\t\t\t\t[TABLE]\n");
            for (k = 0; k < domain->num_of_level; ++k) {
                seq_printf(s, "\t\t\t\t");
                for (l = 0; l < domain->num_of_group; ++l) {
                    if (domain->table_list[j].voltages != NULL)
                        seq_printf(s, "%u ",
                                   domain->table_list[j]
                                       .voltages[k * domain->num_of_group + l]);
                    else if (domain->table_list[j].voltages_step != NULL)
                        seq_printf(
                            s, "%u ",
                            domain->table_list[j]
                                    .voltages_step[k * domain->num_of_group +
                                                   l] *
                                domain->table_list[j].volt_step);
                }
                seq_printf(s, "\n");
            }
        }
    }

    return 0;
}

static int ect_dump_rcc(struct seq_file *s, void *data) {
    int i, j, k, l;
    struct ect_info *info = ect_get_info(BLOCK_RCC);
    struct ect_rcc_header *ect_rcc_header = info->block_handle;
    struct ect_rcc_domain *domain;

    if (ect_rcc_header == NULL) {
        seq_printf(s, "[ECT] : there is no rcc information\n");
        return 0;
    }

    seq_printf(s, "[ECT] : RCC Information\n");
    seq_printf(s, "\t[PARSER VERSION] : %d\n", ect_rcc_header->parser_version);
    seq_printf(s, "\t[VERSION] : %c%c%c%c\n", ect_rcc_header->version[0],
               ect_rcc_header->version[1], ect_rcc_header->version[2],
               ect_rcc_header->version[3]);
    seq_printf(s, "\t[NUM OF DOMAIN] : %d\n", ect_rcc_header->num_of_domain);

    for (i = 0; i < ect_rcc_header->num_of_domain; ++i) {
        domain = &ect_rcc_header->domain_list[i];

        seq_printf(s, "\t\t[DOMAIN NAME] : %s\n", domain->domain_name);
        seq_printf(s, "\t\t[NUM OF ASV GROUP] : %d\n", domain->num_of_group);
        seq_printf(s, "\t\t[NUM OF LEVEL] : %d\n", domain->num_of_level);

        for (j = 0; j < domain->num_of_level; ++j) {
            seq_printf(s, "\t\t\t[FREQUENCY] : %u\n", domain->level_list[j]);
        }

        seq_printf(s, "\t\t[NUM OF TABLE] : %d\n", domain->num_of_table);

        for (j = 0; j < domain->num_of_table; ++j) {
            seq_printf(s, "\t\t\t[TABLE VERSION] : %d\n",
                       domain->table_list[j].table_version);

            seq_printf(s, "\t\t\t\t[TABLE]\n");
            for (k = 0; k < domain->num_of_level; ++k) {
                seq_printf(s, "\t\t\t\t");
                for (l = 0; l < domain->num_of_group; ++l) {
                    if (domain->table_list[j].rcc != NULL)
                        seq_printf(s, "%u ",
                                   domain->table_list[j]
                                       .rcc[k * domain->num_of_group + l]);
                    else if (domain->table_list[j].rcc_compact != NULL)
                        seq_printf(
                            s, "%u ",
                            domain->table_list[j]
                                .rcc_compact[k * domain->num_of_group + l]);
                }
                seq_printf(s, "\n");
            }
        }
    }

    return 0;
}

static int ect_dump_mif_thermal(struct seq_file *s, void *data) {
    int i;
    struct ect_info *info = ect_get_info(BLOCK_MIF_THERMAL);
    struct ect_mif_thermal_header *ect_mif_thermal_header = info->block_handle;
    struct ect_mif_thermal_level *level;

    if (ect_mif_thermal_header == NULL) {
        seq_printf(s, "[ECT] : there is no mif thermal information\n");
        return 0;
    }

    seq_printf(s, "[ECT] : MIF Thermal Information\n");
    seq_printf(s, "\t[PARSER VERSION] : %d\n",
               ect_mif_thermal_header->parser_version);
    seq_printf(
        s, "\t[VERSION] : %c%c%c%c\n", ect_mif_thermal_header->version[0],
        ect_mif_thermal_header->version[1], ect_mif_thermal_header->version[2],
        ect_mif_thermal_header->version[3]);
    seq_printf(s, "\t[NUM OF LEVEL] : %d\n",
               ect_mif_thermal_header->num_of_level);

    for (i = 0; i < ect_mif_thermal_header->num_of_level; ++i) {
        level = &ect_mif_thermal_header->level[i];

        seq_printf(s, "\t\t[MR4 LEVEL] : %d\n", level->mr4_level);
        seq_printf(s, "\t\t[MAX FREQUENCY] : %u\n", level->max_frequency);
        seq_printf(s, "\t\t[MIN FREQUENCY] : %u\n", level->min_frequency);
        seq_printf(s, "\t\t[REFRESH RATE] : %u\n", level->refresh_rate_value);
        seq_printf(s, "\t\t[POLLING PERIOD] : %u\n", level->polling_period);
        seq_printf(s, "\t\t[SW TRIP] : %u\n", level->sw_trip);
    }

    return 0;
}

static int ect_dump_ap_thermal(struct seq_file *s, void *data) {
    int i, j;
    struct ect_info *info = ect_get_info(BLOCK_AP_THERMAL);
    struct ect_ap_thermal_header *ect_ap_thermal_header = info->block_handle;
    struct ect_ap_thermal_function *function;
    struct ect_ap_thermal_range *range;

    if (ect_ap_thermal_header == NULL) {
        seq_printf(s, "[ECT] : there is no ap thermal information\n");
        return 0;
    }

    seq_printf(s, "[ECT] : AP Thermal Information\n");
    seq_printf(s, "\t[PARSER VERSION] : %d\n",
               ect_ap_thermal_header->parser_version);
    seq_printf(s, "\t[VERSION] : %c%c%c%c\n", ect_ap_thermal_header->version[0],
               ect_ap_thermal_header->version[1],
               ect_ap_thermal_header->version[2],
               ect_ap_thermal_header->version[3]);
    seq_printf(s, "\t[NUM OF FUNCTION] : %d\n",
               ect_ap_thermal_header->num_of_function);

    for (i = 0; i < ect_ap_thermal_header->num_of_function; ++i) {
        function = &ect_ap_thermal_header->function_list[i];

        seq_printf(s, "\t\t[FUNCTION NAME] : %s\n", function->function_name);
        seq_printf(s, "\t\t[NUM OF RANGE] : %d\n", function->num_of_range);

        for (j = 0; j < function->num_of_range; ++j) {
            range = &function->range_list[j];

            seq_printf(s, "\t\t\t[LOWER BOUND TEMPERATURE] : %u\n",
                       range->lower_bound_temperature);
            seq_printf(s, "\t\t\t[UPPER BOUND TEMPERATURE] : %u\n",
                       range->upper_bound_temperature);
            seq_printf(s, "\t\t\t[MAX FREQUENCY] : %u\n", range->max_frequency);
            seq_printf(s, "\t\t\t[SW TRIP] : %u\n", range->sw_trip);
            seq_printf(s, "\t\t\t[FLAG] : %u\n", range->flag);
        }
    }

    return 0;
}

static int ect_dump_margin(struct seq_file *s, void *data) {
    int i, j, k;
    struct ect_info *info = ect_get_info(BLOCK_MARGIN);
    struct ect_margin_header *ect_margin_header = info->block_handle;
    struct ect_margin_domain *domain;

    if (ect_margin_header == NULL) {
        seq_printf(s, "[ECT] : there is no margin information\n");
        return 0;
    }

    seq_printf(s, "[ECT] : Margin Information\n");
    seq_printf(s, "\t[PARSER VERSION] : %d\n",
               ect_margin_header->parser_version);
    seq_printf(s, "\t[VERSION] : %c%c%c%c\n", ect_margin_header->version[0],
               ect_margin_header->version[1], ect_margin_header->version[2],
               ect_margin_header->version[3]);
    seq_printf(s, "\t[NUM OF DOMAIN] : %d\n", ect_margin_header->num_of_domain);

    for (i = 0; i < ect_margin_header->num_of_domain; ++i) {
        domain = &ect_margin_header->domain_list[i];

        seq_printf(s, "\t\t[DOMAIN NAME] : %s\n", domain->domain_name);
        seq_printf(s, "\t\t[NUM OF GROUP] : %d\n", domain->num_of_group);
        seq_printf(s, "\t\t[NUM OF LEVEL] : %d\n", domain->num_of_level);

        seq_printf(s, "\t\t\t[TABLE]\n");
        for (j = 0; j < domain->num_of_level; ++j) {
            seq_printf(s, "\t\t\t");
            for (k = 0; k < domain->num_of_group; ++k) {
                if (domain->offset != NULL)
                    seq_printf(s, "%u ",
                               domain->offset[j * domain->num_of_group + k]);
                else if (domain->offset_compact != NULL)
                    seq_printf(
                        s, "%u ",
                        domain->offset_compact[j * domain->num_of_group + k] *
                            domain->volt_step);
            }
            seq_printf(s, "\n");
        }
    }

    return 0;
}

static int ect_dump_timing_parameter(struct seq_file *s, void *data) {
    int i, j, k;
    struct ect_info *info = ect_get_info(BLOCK_TIMING_PARAM);
    struct ect_timing_param_header *ect_timing_param_header =
        info->block_handle;
    struct ect_timing_param_size *size;

    if (ect_timing_param_header == NULL) {
        seq_printf(s, "[ECT] : there is no timing parameter information\n");
        return 0;
    }

    seq_printf(s, "[ECT] : Timing-Parameter Information\n");
    seq_printf(s, "\t[PARSER VERSION] : %d\n",
               ect_timing_param_header->parser_version);
    seq_printf(s, "\t[VERSION] : %c%c%c%c\n",
               ect_timing_param_header->version[0],
               ect_timing_param_header->version[1],
               ect_timing_param_header->version[2],
               ect_timing_param_header->version[3]);
    seq_printf(s, "\t[NUM OF SIZE] : %d\n",
               ect_timing_param_header->num_of_size);

    for (i = 0; i < ect_timing_param_header->num_of_size; ++i) {
        size = &ect_timing_param_header->size_list[i];

        seq_printf(s, "\t\t[PARAMETER KEY] : %p\n",
                   (void *)size->parameter_key);
        seq_printf(s, "\t\t[NUM OF TIMING PARAMETER] : %d\n",
                   size->num_of_timing_param);
        seq_printf(s, "\t\t[NUM OF LEVEL] : %d\n", size->num_of_level);

        seq_printf(s, "\t\t\t[TABLE]\n");
        for (j = 0; j < size->num_of_level; ++j) {
            seq_printf(s, "\t\t\t");
            for (k = 0; k < size->num_of_timing_param; ++k) {
                seq_printf(
                    s, "%X ",
                    size->timing_parameter[j * size->num_of_timing_param + k]);
            }
            seq_printf(s, "\n");
        }
    }

    return 0;
}

static int ect_dump_minlock(struct seq_file *s, void *data) {
    int i, j;
    struct ect_info *info = ect_get_info(BLOCK_MINLOCK);
    struct ect_minlock_header *ect_minlock_header = info->block_handle;
    struct ect_minlock_domain *domain;

    if (ect_minlock_header == NULL) {
        seq_printf(s, "[ECT] : there is no minlock information\n");
        return 0;
    }

    seq_printf(s, "[ECT] : Minlock Information\n");
    seq_printf(s, "\t[PARSER VERSION] : %d\n",
               ect_minlock_header->parser_version);
    seq_printf(s, "\t[VERSION] : %c%c%c%c\n", ect_minlock_header->version[0],
               ect_minlock_header->version[1], ect_minlock_header->version[2],
               ect_minlock_header->version[3]);
    seq_printf(s, "\t[NUM OF DOMAIN] : %d\n",
               ect_minlock_header->num_of_domain);

    for (i = 0; i < ect_minlock_header->num_of_domain; ++i) {
        domain = &ect_minlock_header->domain_list[i];

        seq_printf(s, "\t\t[DOMAIN NAME] : %s\n", domain->domain_name);

        for (j = 0; j < domain->num_of_level; ++j) {
            seq_printf(s, "\t\t\t[Frequency] : (MAIN)%u, (SUB)%u\n",
                       domain->level[j].main_frequencies,
                       domain->level[j].sub_frequencies);
        }
    }

    return 0;
}

static int ect_dump_gen_parameter(struct seq_file *s, void *data) {
    int i, j, k;
    struct ect_info *info = ect_get_info(BLOCK_GEN_PARAM);
    struct ect_gen_param_header *ect_gen_param_header = info->block_handle;
    struct ect_gen_param_table *table;

    if (ect_gen_param_header == NULL) {
        seq_printf(s, "[ECT] : there is no general parameter information\n");
        return 0;
    }

    seq_printf(s, "[ECT] : General-Parameter Information\n");
    seq_printf(s, "\t[PARSER VERSION] : %d\n",
               ect_gen_param_header->parser_version);
    seq_printf(s, "\t[VERSION] : %c%c%c%c\n", ect_gen_param_header->version[0],
               ect_gen_param_header->version[1],
               ect_gen_param_header->version[2],
               ect_gen_param_header->version[3]);
    seq_printf(s, "\t[NUM OF TABLE] : %d\n",
               ect_gen_param_header->num_of_table);

    for (i = 0; i < ect_gen_param_header->num_of_table; ++i) {
        table = &ect_gen_param_header->table_list[i];

        seq_printf(s, "\t\t[TABLE NAME] : %s\n", table->table_name);
        seq_printf(s, "\t\t[NUM OF COLUMN] : %d\n", table->num_of_col);
        seq_printf(s, "\t\t[NUM OF ROW] : %d\n", table->num_of_row);

        seq_printf(s, "\t\t\t[TABLE]\n");
        for (j = 0; j < table->num_of_row; ++j) {
            seq_printf(s, "\t\t\t");
            for (k = 0; k < table->num_of_col; ++k) {
                seq_printf(s, "%u ",
                           table->parameter[j * table->num_of_col + k]);
            }
            seq_printf(s, "\n");
        }
    }

    return 0;
}

static int ect_dump_binary(struct seq_file *s, void *data) {
    int i, j, crc;
    struct ect_info *info = ect_get_info(BLOCK_BIN);
    struct ect_bin_header *ect_binary_header = info->block_handle;
    struct ect_bin *bin;
    char *data_ptr;

    if (ect_binary_header == NULL) {
        seq_printf(s, "[ECT] : there is no binary information\n");
        return 0;
    }

    seq_printf(s, "[ECT] : Binary Information\n");
    seq_printf(s, "\t[PARSER VERSION] : %d\n",
               ect_binary_header->parser_version);
    seq_printf(s, "\t[VERSION] : %c%c%c%c\n", ect_binary_header->version[0],
               ect_binary_header->version[1], ect_binary_header->version[2],
               ect_binary_header->version[3]);
    seq_printf(s, "\t[NUM OF BINARY] : %d\n", ect_binary_header->num_of_binary);

    for (i = 0; i < ect_binary_header->num_of_binary; ++i) {
        bin = &ect_binary_header->binary_list[i];

        seq_printf(s, "\t\t[BINARY NAME] : %s\n", bin->binary_name);

        crc = 0;
        data_ptr = bin->ptr;
        for (j = 0; j < bin->binary_size; ++j) {
            crc ^= data_ptr[j] << (j & 31);
        }
        seq_printf(s, "\t\t\t[BINARY CRC] : %x\n", crc);
    }

    return 0;
}

static int ect_dump_new_timing_parameter(struct seq_file *s, void *data) {
    int i, j, k;
    struct ect_info *info = ect_get_info(BLOCK_NEW_TIMING_PARAM);
    struct ect_new_timing_param_header *ect_new_timing_param_header =
        info->block_handle;
    struct ect_new_timing_param_size *size;

    if (ect_new_timing_param_header == NULL) {
        seq_printf(s, "[ECT] : there is no new timing parameter information\n");
        return 0;
    }

    seq_printf(s, "[ECT] : New Timing-Parameter Information\n");
    seq_printf(s, "\t[PARSER VERSION] : %d\n",
               ect_new_timing_param_header->parser_version);
    seq_printf(s, "\t[VERSION] : %c%c%c%c\n",
               ect_new_timing_param_header->version[0],
               ect_new_timing_param_header->version[1],
               ect_new_timing_param_header->version[2],
               ect_new_timing_param_header->version[3]);
    seq_printf(s, "\t[NUM OF SIZE] : %d\n",
               ect_new_timing_param_header->num_of_size);

    for (i = 0; i < ect_new_timing_param_header->num_of_size; ++i) {
        size = &ect_new_timing_param_header->size_list[i];

        seq_printf(s, "\t\t[PARAMETER KEY] : %llX\n", size->parameter_key);
        seq_printf(s, "\t\t[NUM OF TIMING PARAMETER] : %d\n",
                   size->num_of_timing_param);
        seq_printf(s, "\t\t[NUM OF LEVEL] : %d\n", size->num_of_level);

        seq_printf(s, "\t\t\t[TABLE]\n");
        for (j = 0; j < size->num_of_level; ++j) {
            seq_printf(s, "\t\t\t");
            for (k = 0; k < size->num_of_timing_param; ++k) {
                if (size->mode == e_mode_normal_value)
                    seq_printf(
                        s, "%X ",
                        size->timing_parameter[j * size->num_of_timing_param +
                                               k]);
                else if (size->mode == e_mode_extend_value)
                    seq_printf(
                        s, "%llX ",
                        ect_read_value64(size->timing_parameter,
                                         j * size->num_of_timing_param + k));
            }
            seq_printf(s, "\n");
        }
    }

    return 0;
}

static int ect_dump_pidtm(struct seq_file *s, void *data) {
    int i, j;
    struct ect_info *info = ect_get_info(BLOCK_PIDTM);
    struct ect_pidtm_header *ect_pidtm_header = info->block_handle;
    struct ect_pidtm_block *block;

    if (ect_pidtm_header == NULL) {
        seq_printf(s, "[ECT] : there is no pidtm parameter information\n");
        return 0;
    }

    seq_printf(s, "[ECT] : PIDTM Parameter Information\n");
    seq_printf(s, "\t[PARSER VERSION] : %d\n",
               ect_pidtm_header->parser_version);
    seq_printf(s, "\t[VERSION] : %c%c%c%c\n", ect_pidtm_header->version[0],
               ect_pidtm_header->version[1], ect_pidtm_header->version[2],
               ect_pidtm_header->version[3]);
    seq_printf(s, "\t[NUM OF BLOCK] : %d\n", ect_pidtm_header->num_of_block);

    for (i = 0; i < ect_pidtm_header->num_of_block; ++i) {
        block = &ect_pidtm_header->block_list[i];

        seq_printf(s, "\t\t[BLOCK NAME] : %s\n", block->block_name);
        seq_printf(s, "\t\t[NUM OF TEMPERATURE] : %d\n",
                   block->num_of_temperature);

        for (j = 0; j < block->num_of_temperature; ++j) {
            seq_printf(s, "\t\t\t[TRIGGER TEMPERATURE] : %d\n",
                       block->temperature_list[j]);
        }

        seq_printf(s, "\t\t[NUM OF PARAMETER] : %d\n", block->num_of_parameter);
        for (j = 0; j < block->num_of_parameter; ++j) {
            seq_printf(s, "\t\t\t[PARAMETER] : %s, %d\n",
                       block->param_name_list[j], block->param_value_list[j]);
        }
    }

    return 0;
}

static int dump_open(struct inode *inode, struct file *file) {
    struct ect_info *info = (struct ect_info *)inode->i_private;

    return single_open(file, info->dump, inode->i_private);
}

static int ect_dump_all(struct seq_file *s, void *data) {
    int i, j, ret;

    ret = ect_header_info.dump(s, data);
    if (ret)
        return ret;

    for (i = 0; i < ARRAY_SIZE32(ect_list); ++i) {
        for (j = 0; j < ARRAY_SIZE32(ect_list); ++j) {
            if (ect_list[j].block_precedence != i)
                continue;

            ret = ect_list[j].dump(s, data);
            if (ret)
                return ret;
        }
    }

    return 0;
}

static int dump_all_open(struct inode *inode, struct file *file) {
    return single_open(file, ect_dump_all, inode->i_private);
}

static struct file_operations ops_all_dump = {
    .open = dump_all_open,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};

static ssize_t ect_raw_blob_read(struct file *file, char __user *user_buf,
                                 size_t count, loff_t *ppos) {
    void *base;
    phys_addr_t phys;
    size_t size;
    ssize_t ret;

    if (!ect_early_vm.phys_addr || !ect_early_vm.size)
        return -ENODEV;

    phys = ect_early_vm.phys_addr;
    size = ect_early_vm.size;

    base = memremap(phys, size, MEMREMAP_WB);
    if (!base) {
        pr_err("[ect-raw] failed to remap 0x%llx (size 0x%zx)\n",
               (unsigned long long)phys, size);
        return -ENOMEM;
    }

    ret = simple_read_from_buffer(user_buf, count, ppos, base, size);

    memunmap(base);

    return ret;
}

static const struct file_operations ops_raw_blob_dump = {
    .read = ect_raw_blob_read,
    .llseek = default_llseek,
};

static ssize_t create_binary_store(struct class *class,
                                   struct class_attribute *attr,
                                   const char *buf, size_t size) {
    char filename_buffer[512];
    long pattern_fd;
    mm_segment_t old_fs;
    struct file *fp;
    loff_t pos = 0;
    int ret;

    ret = sscanf(buf, "%511s", filename_buffer);
    if (ret != 1)
        return -EINVAL;

    old_fs = get_fs();
    set_fs(KERNEL_DS);

    pattern_fd =
        do_sys_open(AT_FDCWD, filename_buffer,
                    O_WRONLY | O_CREAT | O_TRUNC | O_SYNC | O_NOFOLLOW, 0664);
    if (pattern_fd < 0) {
        pr_err("[ECT] : error to open file\n");
        set_fs(old_fs);
        return -EINVAL;
    }

    fp = fget(pattern_fd);
    if (fp) {
        vfs_write(fp, (const char *)ect_address, ect_size, &pos);
        vfs_fsync(fp, 0);
        fput(fp);
    } else {
        pr_err("[ECT] : error to convert file\n");
    }

    get_close_on_exec(pattern_fd);
    set_fs(old_fs);

    return size;
}

static CLASS_ATTR_WO(create_binary);

static int ect_dump_init(void) {
    int i;
    struct dentry *root, *d;

    root = debugfs_create_dir("ect", NULL);
    if (!root) {
        pr_err("%s: couln't create debugfs\n", __FILE__);
        return -ENOMEM;
    }

    d = debugfs_create_file("all_dump", S_IRUGO, root, NULL, &ops_all_dump);
    if (!d)
        return -ENOMEM;

    d = debugfs_create_file("raw_blob", S_IRUGO, root, NULL,
                            &ops_raw_blob_dump);
    if (!d)
        return -ENOMEM;

    d = debugfs_create_file(ect_header_info.dump_node_name, S_IRUGO, root,
                            &ect_header_info, &ect_header_info.dump_ops);
    if (!d)
        return -ENOMEM;

    for (i = 0; i < ARRAY_SIZE32(ect_list); ++i) {
        if (ect_list[i].block_handle == NULL)
            continue;

        d = debugfs_create_file(ect_list[i].dump_node_name, S_IRUGO, root,
                                &(ect_list[i]), &ect_list[i].dump_ops);
        if (!d)
            return -ENOMEM;
    }

    ect_class = class_create(THIS_MODULE, "ect");
    if (IS_ERR(ect_class)) {
        pr_err("%s: couldn't create class\n", __FILE__);
        return PTR_ERR(ect_class);
    }

    if (class_create_file(ect_class, &class_attr_create_binary)) {
        pr_err("%s: couldn't create generate_data node\n", __FILE__);
        return -EINVAL;
    }

    return 0;
}
late_initcall_sync(ect_dump_init);
#endif

/* API for external */

void __init ect_init(phys_addr_t address, phys_addr_t size) {
    ect_early_vm.phys_addr = address;
    ect_early_vm.addr = (void *)S5P_VA_ECT;
    ect_early_vm.size = size;

    vm_area_add_early(&ect_early_vm);

    ect_address = (phys_addr_t)S5P_VA_ECT;
    ect_size = size;
}

unsigned long long ect_read_value64(unsigned int *address, int index) {
    unsigned int top, half;

    index *= 2;

    half = address[index];
    top = address[index + 1];

    return ((unsigned long long)top << 32 | half);
}

void *ect_get_block(char *block_name) {
    int i;

    for (i = 0; i < ARRAY_SIZE32(ect_list); ++i) {
        if (ect_strcmp(block_name, ect_list[i].block_name) == 0)
            return ect_list[i].block_handle;
    }

    return NULL;
}

struct ect_dvfs_domain *ect_dvfs_get_domain(void *block, char *domain_name) {
    int i;
    struct ect_dvfs_header *header;
    struct ect_dvfs_domain *domain;

    if (block == NULL || domain_name == NULL)
        return NULL;

    header = (struct ect_dvfs_header *)block;

    for (i = 0; i < header->num_of_domain; ++i) {
        domain = &header->domain_list[i];

        if (ect_strcmp(domain_name, domain->domain_name) == 0)
            return domain;
    }

    return NULL;
}

struct ect_pll *ect_pll_get_pll(void *block, char *pll_name) {
    int i;
    struct ect_pll_header *header;
    struct ect_pll *pll;

    if (block == NULL || pll_name == NULL)
        return NULL;

    header = (struct ect_pll_header *)block;

    for (i = 0; i < header->num_of_pll; ++i) {
        pll = &header->pll_list[i];

        if (ect_strcmp(pll_name, pll->pll_name) == 0)
            return pll;
    }

    return NULL;
}

struct ect_voltage_domain *ect_asv_get_domain(void *block, char *domain_name) {
    int i;
    struct ect_voltage_header *header;
    struct ect_voltage_domain *domain;

    if (block == NULL || domain_name == NULL)
        return NULL;

    header = (struct ect_voltage_header *)block;

    for (i = 0; i < header->num_of_domain; ++i) {
        domain = &header->domain_list[i];

        if (ect_strcmp(domain_name, domain->domain_name) == 0)
            return domain;
    }

    return NULL;
}

struct ect_rcc_domain *ect_rcc_get_domain(void *block, char *domain_name) {
    int i;
    struct ect_rcc_header *header;
    struct ect_rcc_domain *domain;

    if (block == NULL || domain_name == NULL)
        return NULL;

    header = (struct ect_rcc_header *)block;

    for (i = 0; i < header->num_of_domain; ++i) {
        domain = &header->domain_list[i];

        if (ect_strcmp(domain_name, domain->domain_name) == 0)
            return domain;
    }

    return NULL;
}

struct ect_mif_thermal_level *ect_mif_thermal_get_level(void *block,
                                                        int mr4_level) {
    int i;
    struct ect_mif_thermal_header *header;
    struct ect_mif_thermal_level *level;

    if (block == NULL)
        return NULL;

    header = (struct ect_mif_thermal_header *)block;

    for (i = 0; i < header->num_of_level; ++i) {
        level = &header->level[i];

        if (level->mr4_level == mr4_level)
            return level;
    }

    return NULL;
}

struct ect_ap_thermal_function *
ect_ap_thermal_get_function(void *block, char *function_name) {
    int i;
    struct ect_ap_thermal_header *header;
    struct ect_ap_thermal_function *function;

    if (block == NULL || function_name == NULL)
        return NULL;

    header = (struct ect_ap_thermal_header *)block;

    for (i = 0; i < header->num_of_function; ++i) {
        function = &header->function_list[i];

        if (ect_strcmp(function_name, function->function_name) == 0)
            return function;
    }

    return NULL;
}

struct ect_pidtm_block *ect_pidtm_get_block(void *block, char *block_name) {
    int i;
    struct ect_pidtm_header *header;
    struct ect_pidtm_block *pidtm_block;

    if (block == NULL || block_name == NULL)
        return NULL;

    header = (struct ect_pidtm_header *)block;

    for (i = 0; i < header->num_of_block; ++i) {
        pidtm_block = &header->block_list[i];

        if (ect_strcmp(block_name, pidtm_block->block_name) == 0)
            return pidtm_block;
    }

    return NULL;
}

struct ect_margin_domain *ect_margin_get_domain(void *block,
                                                char *domain_name) {
    int i;
    struct ect_margin_header *header;
    struct ect_margin_domain *domain;

    if (block == NULL || domain_name == NULL)
        return NULL;

    header = (struct ect_margin_header *)block;

    for (i = 0; i < header->num_of_domain; ++i) {
        domain = &header->domain_list[i];

        if (ect_strcmp(domain_name, domain->domain_name) == 0)
            return domain;
    }

    return NULL;
}

struct ect_timing_param_size *ect_timing_param_get_size(void *block,
                                                        int dram_size) {
    int i;
    struct ect_timing_param_header *header;
    struct ect_timing_param_size *size;

    if (block == NULL)
        return NULL;

    header = (struct ect_timing_param_header *)block;

    for (i = 0; i < header->num_of_size; ++i) {
        size = &header->size_list[i];

        if (size->memory_size == dram_size)
            return size;
    }

    return NULL;
}

struct ect_timing_param_size *ect_timing_param_get_key(void *block,
                                                       unsigned long long key) {
    int i;
    struct ect_timing_param_header *header;
    struct ect_timing_param_size *size;

    if (block == NULL)
        return NULL;

    header = (struct ect_timing_param_header *)block;

    for (i = 0; i < header->num_of_size; ++i) {
        size = &header->size_list[i];

        if (key == size->parameter_key)
            return size;
    }

    return NULL;
}

struct ect_minlock_domain *ect_minlock_get_domain(void *block,
                                                  char *domain_name) {
    int i;
    struct ect_minlock_header *header;
    struct ect_minlock_domain *domain;

    if (block == NULL || domain_name == NULL)
        return NULL;

    header = (struct ect_minlock_header *)block;

    for (i = 0; i < header->num_of_domain; ++i) {
        domain = &header->domain_list[i];

        if (ect_strcmp(domain_name, domain->domain_name) == 0)
            return domain;
    }

    return NULL;
}

struct ect_gen_param_table *ect_gen_param_get_table(void *block,
                                                    char *table_name) {
    int i;
    struct ect_gen_param_header *header;
    struct ect_gen_param_table *table;

    if (block == NULL)
        return NULL;

    header = (struct ect_gen_param_header *)block;

    for (i = 0; i < header->num_of_table; ++i) {
        table = &header->table_list[i];

        if (ect_strcmp(table->table_name, table_name) == 0)
            return table;
    }

    return NULL;
}

struct ect_bin *ect_binary_get_bin(void *block, char *binary_name) {
    int i;
    struct ect_bin_header *header;
    struct ect_bin *bin;

    if (block == NULL)
        return NULL;

    header = (struct ect_bin_header *)block;

    for (i = 0; i < header->num_of_binary; ++i) {
        bin = &header->binary_list[i];

        if (ect_strcmp(bin->binary_name, binary_name) == 0)
            return bin;
    }

    return NULL;
}

struct ect_new_timing_param_size *
ect_new_timing_param_get_key(void *block, unsigned long long key) {
    int i;
    struct ect_new_timing_param_header *header;
    struct ect_new_timing_param_size *size;

    if (block == NULL)
        return NULL;

    header = (struct ect_new_timing_param_header *)block;

    for (i = 0; i < header->num_of_size; ++i) {
        size = &header->size_list[i];

        if (key == size->parameter_key)
            return size;
    }

    return NULL;
}

static int ect_override_g3d_tables(void) {
    void *dvfs_blk, *asv_blk, *gen_blk;
    struct ect_dvfs_domain *dvfs;
    struct ect_voltage_domain *asv;
    struct ect_gen_param_table *margin_tbl;

    int old_levels;

    /* Ziel-Liste: kHz (DVFS) + MHz (ASV) muss konsistent sein */
    static const u32 freqs_khz[] = {
        910000, 858000, 806000, 754000, 702000, 676000, 650000, 598000,
        572000, 433000, 377000, 325000, 260000, 200000, 156000, 100000};
    static const int32_t freqs_mhz[] = {910, 858, 806, 754, 702, 676, 650, 598,
                                        572, 433, 377, 325, 260, 200, 156, 100};

    const int new_levels = ARRAY_SIZE(freqs_khz);

    /* --- DVFS domain holen --- */
    dvfs_blk = ect_get_block(BLOCK_DVFS);
    if (!dvfs_blk)
        return -ENODEV;

    dvfs = ect_dvfs_get_domain(dvfs_blk, "dvfs_g3d");
    if (!dvfs)
        return -ENODEV;

    old_levels = dvfs->num_of_level;

    if (old_levels >= new_levels) {
        pr_info("[ECT] g3d override: already %d levels\n", old_levels);
    } else {
        /* list_level neu */
        {
            void *new_list_level;
            u32 *p;
            int i;
            const size_t stride_u32 =
                sizeof(struct ect_dvfs_level) / sizeof(u32);

            new_list_level =
                kzalloc(sizeof(struct ect_dvfs_level) * new_levels, GFP_KERNEL);
            if (!new_list_level)
                return -ENOMEM;

            p = (u32 *)new_list_level;

            for (i = 0; i < new_levels; i++) {
                p[i * stride_u32 + 0] = freqs_khz[i]; /* freq kHz */
                p[i * stride_u32 + 1] = 1;            /* enable */
            }

            dvfs->list_level = new_list_level;
        }

        /* list_dvfs_value neu (Index-Mapping pro Clock) */
        {
            u32 *new_map;
            int c, i;
            const int clocks = dvfs->num_of_clock;

            new_map = kzalloc(sizeof(u32) * clocks * new_levels, GFP_KERNEL);
            if (!new_map)
                return -ENOMEM;

            for (c = 0; c < clocks; c++) {
                for (i = 0; i < new_levels; i++)
                    new_map[c * new_levels + i] = (u32)i;
            }

            dvfs->list_dvfs_value = new_map;
        }

        dvfs->num_of_level = new_levels;
        dvfs->max_frequency = freqs_khz[0];
        dvfs->min_frequency = freqs_khz[new_levels - 1];

        pr_info("[ECT] g3d override: DVFS levels %d -> %d\n", old_levels,
                new_levels);
    }

    /* --- ASV domain holen --- */
    asv_blk = ect_get_block(BLOCK_ASV);
    if (!asv_blk)
        return -ENODEV;

    asv = ect_asv_get_domain(asv_blk, "dvfs_g3d");
    if (!asv)
        return -ENODEV;

    old_levels = asv->num_of_level;

    if (old_levels < new_levels) {
        const int delta = new_levels - old_levels;
        const int g = asv->num_of_group;
        int t;

        /* 1) level_list (MHz) neu */
        {
            int32_t *new_level_list;

            new_level_list = kzalloc(sizeof(int32_t) * new_levels, GFP_KERNEL);
            if (!new_level_list)
                return -ENOMEM;

            memcpy(new_level_list, freqs_mhz, sizeof(freqs_mhz));
            asv->level_list = new_level_list;
        }

        /* 2) jede TABLE VERSION aufblasen:
              neue Top-Rows = Kopie der alten Top-Row (konservativ)
              alte Rows werden um delta nach unten geschoben */
        for (t = 0; t < asv->num_of_table; t++) {
            struct ect_voltage_table *tbl = &asv->table_list[t];

            /* level_en erweitern (wenn vorhanden) */
            if (tbl->level_en) {
                int32_t *old_en = (int32_t *)tbl->level_en;
                int32_t *new_en =
                    kzalloc(sizeof(int32_t) * new_levels, GFP_KERNEL);
                int r;

                if (!new_en)
                    return -ENOMEM;

                for (r = 0; r < delta; r++)
                    new_en[r] = old_en[0];

                memcpy(&new_en[delta], old_en, sizeof(int32_t) * old_levels);
                tbl->level_en = new_en;
            }

            /* parser_version>=3: voltages_step (u8) */
            if (tbl->voltages_step) {
                u8 *old = (u8 *)tbl->voltages_step;
                u8 *neu = kzalloc(sizeof(u8) * g * new_levels, GFP_KERNEL);
                int r;

                if (!neu)
                    return -ENOMEM;

                /* neue Top-Rows (0..delta-1) = alte Row0 */
                for (r = 0; r < delta; r++)
                    memcpy(&neu[g * r], &old[0], g * sizeof(u8));

                /* alte Rows nach unten schieben */
                memcpy(&neu[g * delta], &old[0], g * old_levels * sizeof(u8));

                tbl->voltages_step = neu;
            }
            /* parser_version<3: voltages (int32 uV) */
            else if (tbl->voltages) {
                int32_t *old = (int32_t *)tbl->voltages;
                int32_t *neu =
                    kzalloc(sizeof(int32_t) * g * new_levels, GFP_KERNEL);
                int r;

                if (!neu)
                    return -ENOMEM;

                for (r = 0; r < delta; r++)
                    memcpy(&neu[g * r], &old[0], g * sizeof(int32_t));

                memcpy(&neu[g * delta], &old[0],
                       g * old_levels * sizeof(int32_t));

                tbl->voltages = neu;
            } else {
                pr_warn(
                    "[ECT] g3d override: ASV table %d has no voltage data\n",
                    t);
            }
        }

        asv->num_of_level = new_levels;

        pr_info("[ECT] g3d override: ASV levels %d -> %d (delta=%d)\n",
                old_levels, new_levels, delta);
    }

    /* --- GEN_PARAM: G3D_DD_margin aufblasen (auf new_levels) --- */
    gen_blk = ect_get_block(BLOCK_GEN_PARAM);
    if (!gen_blk)
        return 0; /* nicht fatal */

    margin_tbl = ect_gen_param_get_table(gen_blk, "G3D_DD_margin");
    if (margin_tbl) {
        const int cols = margin_tbl->num_of_col;
        const int rows = margin_tbl->num_of_row;

        if (cols == 2 && rows > 0 && rows < new_levels &&
            margin_tbl->parameter) {
            int32_t *oldp = (int32_t *)margin_tbl->parameter;
            int32_t *newp =
                kzalloc(sizeof(int32_t) * cols * new_levels, GFP_KERNEL);
            const int delta = new_levels - rows;
            int i;
            int32_t top_margin = oldp[1];

            if (!newp)
                return -ENOMEM;

            if (top_margin == 0)
                top_margin = 12500;

            /* neue Top-Rows: konservativ gleicher Margin wie alte Top-Row */
            for (i = 0; i < delta; i++) {
                newp[i * 2 + 0] = i;
                newp[i * 2 + 1] = top_margin;
            }

            /* alte Rows nach unten schieben, Margin aus alter 2. Spalte
             * übernehmen */
            for (i = 0; i < rows; i++) {
                newp[(i + delta) * 2 + 0] = i + delta;
                newp[(i + delta) * 2 + 1] = oldp[i * 2 + 1];
            }

            margin_tbl->parameter = newp;
            margin_tbl->num_of_row = new_levels;

            pr_info("[ECT] g3d override: G3D_DD_margin rows %d -> %d\n", rows,
                    new_levels);
        }
    }

    return 0;
}

static int ect_override_g3d_pll_table(void) {
    static const struct ect_pll_frequency desired[] = {
        {.frequency = 910000000, .p = 4, .m = 140, .s = 0, .k = 0},
        {.frequency = 858000000, .p = 4, .m = 132, .s = 0, .k = 0},
        {.frequency = 806000000, .p = 4, .m = 124, .s = 0, .k = 0},
        {.frequency = 754000000, .p = 4, .m = 116, .s = 0, .k = 0},
        {.frequency = 702000000, .p = 4, .m = 108, .s = 0, .k = 0},
        {.frequency = 676000000, .p = 4, .m = 104, .s = 0, .k = 0},
        {.frequency = 650000000, .p = 4, .m = 100, .s = 0, .k = 0},
        {.frequency = 598000000, .p = 4, .m = 184, .s = 1, .k = 0},
        {.frequency = 572000000, .p = 4, .m = 176, .s = 1, .k = 0},
        {.frequency = 432250000, .p = 4, .m = 133, .s = 1, .k = 0},
        {.frequency = 377000000, .p = 4, .m = 116, .s = 1, .k = 0},
        {.frequency = 325000000, .p = 4, .m = 100, .s = 1, .k = 0},
        {.frequency = 260000000, .p = 4, .m = 160, .s = 2, .k = 0},
        {.frequency = 199875000, .p = 4, .m = 123, .s = 2, .k = 0},
        {.frequency = 156000000, .p = 4, .m = 96,  .s = 2, .k = 0},
        {.frequency = 99937000,  .p = 4, .m = 123, .s = 3, .k = 0},
    };
    void *pll_blk;
    struct ect_pll *pll;
    struct ect_pll_frequency *new_list;
    bool present[ARRAY_SIZE(desired)] = {false};
    int old_n, new_n;
    int missing = 0;
    int i, idx;

    pll_blk = ect_get_block(BLOCK_PLL);
    if (!pll_blk)
        return -ENODEV;

    pll = ect_pll_get_pll(pll_blk, "PLL_G3D");
    if (!pll)
        return -ENODEV;

    old_n = pll->num_of_frequency;

    for (i = 0; i < ARRAY_SIZE(desired); i++) {
        int j;

        if (pll->frequency_list) {
            for (j = 0; j < old_n; j++) {
                if (pll->frequency_list[j].frequency == desired[i].frequency) {
                    present[i] = true;
                    break;
                }
            }
        }

        if (!present[i])
            missing++;
    }

    if (!missing) {
        pr_info(
            "[ECT] g3d override: PLL_G3D already has all %zu target freqs\n",
            ARRAY_SIZE(desired));
        return 0;
    }

    new_n = old_n + missing;

    new_list = kzalloc(sizeof(*new_list) * new_n, GFP_KERNEL);
    if (!new_list)
        return -ENOMEM;

    /* Prepend the missing targets first to preserve priority order */
    idx = 0;
    for (i = 0; i < ARRAY_SIZE(desired); i++) {
        if (present[i])
            continue;
        new_list[idx++] = desired[i];
    }

    /* Rest 1:1 übernehmen */
    if (pll->frequency_list && old_n > 0)
        memcpy(&new_list[idx], pll->frequency_list, sizeof(*new_list) * old_n);

    /* Replace pointer */
    kfree(pll->frequency_list);
    pll->frequency_list = new_list;
    pll->num_of_frequency = new_n;

    pr_info("[ECT] g3d override: PLL_G3D freqs %d -> %d (added %d entries)\n",
            old_n, new_n, missing);

    return 0;
}

static void ect_print_dvfs_block(struct ect_dvfs_header *h) {
    int i, j, k;

    pr_info("[ECT] DVFS: parser=%d ver=%c%c%c%c domains=%d\n",
            h->parser_version, h->version[0], h->version[1], h->version[2],
            h->version[3], h->num_of_domain);

    for (i = 0; i < h->num_of_domain; i++) {
        struct ect_dvfs_domain *d = &h->domain_list[i];

        pr_info("[ECT]  DVFS domain=%s max=%u min=%u boot_idx=%d resume_idx=%d "
                "mode=0x%x clocks=%d levels=%d\n",
                d->domain_name, d->max_frequency, d->min_frequency,
                d->boot_level_idx, d->resume_level_idx, d->mode,
                d->num_of_clock, d->num_of_level);

        if (d->mode == e_dvfs_mode_clock_name && d->list_clock) {
            for (j = 0; j < d->num_of_clock; j++)
                pr_info("[ECT]    clock[%d]=%s\n", j, d->list_clock[j]);
        } else if (d->mode == e_dvfs_mode_sfr_address && d->list_sfr) {
            for (j = 0; j < d->num_of_clock; j++)
                pr_info("[ECT]    sfr[%d]=0x%x\n", j, d->list_sfr[j]);
        }

        for (j = 0; j < d->num_of_level; j++) {
            pr_info("[ECT]    level[%d]=%u en=%d\n", j, d->list_level[j].level,
                    d->list_level[j].level_en);
        }

        /* Table: level-major, num_of_clock columns */
        for (j = 0; j < d->num_of_level; j++) {
            pr_info("[ECT]    table L%d:\n", j);
            for (k = 0; k < d->num_of_clock; k++) {
                unsigned int v = d->list_dvfs_value[j * d->num_of_clock + k];
                pr_info("[ECT]      [%d,%d]=%u\n", j, k, v);
            }
        }
    }
}

static void ect_print_asv_block(struct ect_voltage_header *h) {
    int i, j, k, g;

    pr_info("[ECT] ASV: parser=%d ver=%c%c%c%c domains=%d\n", h->parser_version,
            h->version[0], h->version[1], h->version[2], h->version[3],
            h->num_of_domain);

    for (i = 0; i < h->num_of_domain; i++) {
        struct ect_voltage_domain *d = &h->domain_list[i];
        pr_info("[ECT]  ASV domain=%s groups=%d levels=%d tables=%d\n",
                d->domain_name, d->num_of_group, d->num_of_level,
                d->num_of_table);

        for (j = 0; j < d->num_of_level; j++)
            pr_info("[ECT]    freq[%d]=%u\n", j, d->level_list[j]);

        for (k = 0; k < d->num_of_table; k++) {
            struct ect_voltage_table *t = &d->table_list[k];
            pr_info("[ECT]    table[%d] ver=%d boot_idx=%d resume_idx=%d "
                    "volt_step=%u\n",
                    k, t->table_version, t->boot_level_idx, t->resume_level_idx,
                    t->volt_step);

            if (t->level_en) {
                for (j = 0; j < d->num_of_level; j++)
                    pr_info("[ECT]      en[%d]=%d\n", j, t->level_en[j]);
            }

            for (j = 0; j < d->num_of_level; j++) {
                for (g = 0; g < d->num_of_group; g++) {
                    unsigned int uv = 0;

                    if (t->voltages)
                        uv = t->voltages[j * d->num_of_group + g];
                    else if (t->voltages_step)
                        uv = t->voltages_step[j * d->num_of_group + g] *
                             t->volt_step;

                    pr_info("[ECT]      V[%d,%d]=%u uV\n", j, g, uv);
                }
            }
        }
    }
}

static void ect_print_pll_block(struct ect_pll_header *h) {
    int i, j;

    pr_info("[ECT] PLL: parser=%d ver=%c%c%c%c plls=%d\n", h->parser_version,
            h->version[0], h->version[1], h->version[2], h->version[3],
            h->num_of_pll);

    for (i = 0; i < h->num_of_pll; i++) {
        struct ect_pll *p = &h->pll_list[i];
        pr_info("[ECT]  pll=%s type=%u freqs=%d\n", p->pll_name, p->type_pll,
                p->num_of_frequency);

        for (j = 0; j < p->num_of_frequency; j++) {
            struct ect_pll_frequency *f = &p->frequency_list[j];
            pr_info("[ECT]    f[%d]=%u p=%u m=%u s=%u k=%u\n", j, f->frequency,
                    f->p, f->m, f->s, f->k);
        }
    }
}

static void ect_print_all_blocks_once(void) {
    static bool done;

    struct ect_header *hdr = ect_header_info.block_handle;
    void *blk;

    if (done)
        return;
    done = true;

    pr_info("====================================\n");
    pr_info("[ECT] FULL DUMP (printed at parse end)\n");

    if (hdr) {
        pr_info("[ECT] HEADER: VA=%p SIGN=%c%c%c%c VER=%c%c%c%c total=%u "
                "headers=%d\n",
                (void *)S5P_VA_ECT, hdr->sign[0], hdr->sign[1], hdr->sign[2],
                hdr->sign[3], hdr->version[0], hdr->version[1], hdr->version[2],
                hdr->version[3], hdr->total_size, hdr->num_of_header);
    } else {
        pr_info("[ECT] HEADER: (null)\n");
    }

    blk = ect_get_block(BLOCK_DVFS);
    if (blk)
        ect_print_dvfs_block((struct ect_dvfs_header *)blk);

    blk = ect_get_block(BLOCK_ASV);
    if (blk)
        ect_print_asv_block((struct ect_voltage_header *)blk);

    blk = ect_get_block(BLOCK_PLL);
    if (blk)
        ect_print_pll_block((struct ect_pll_header *)blk);

    /* Optional: raw bytes preview (keep small!) */
    pr_info("[ECT] RAW (first 256 bytes):\n");
    print_hex_dump(KERN_INFO, "[ECT] ", DUMP_PREFIX_OFFSET, 16, 4,
                   (void *)ect_address, min_t(size_t, ect_size, 256), false);

    pr_info("====================================\n");
}

int ect_parse_binary_header(void) {
    int ret = 0;
    int i, j;
    char *block_name;
    void *address;
    unsigned int length, offset;
    struct ect_header *ect_header;

    ect_init_map_io();

    address = (void *)ect_address;
    if (address == NULL)
        return -EINVAL;

    ect_header = kzalloc(sizeof(struct ect_header), GFP_KERNEL);

    ect_parse_integer(&address, ect_header->sign);
    ect_parse_integer(&address, ect_header->version);
    ect_parse_integer(&address, &ect_header->total_size);
    ect_parse_integer(&address, &ect_header->num_of_header);

    if (memcmp(ect_header->sign, ect_signature, sizeof(ect_signature) - 1)) {
        ret = -EINVAL;
        goto err_memcmp;
    }

    ect_present_test_data(ect_header->version);

    for (i = 0; i < ect_header->num_of_header; ++i) {
        if (ect_parse_string(&address, &block_name, &length)) {
            ret = -EINVAL;
            goto err_parse_string;
        }

        ect_parse_integer(&address, &offset);

        for (j = 0; j < ARRAY_SIZE32(ect_list); ++j) {
            if (strncmp(block_name, ect_list[j].block_name,
                        ect_list[j].block_name_length) != 0)
                continue;

            if (ect_list[j].parser((void *)ect_address + offset,
                                   ect_list + j)) {
                pr_err("[ECT] : parse error %s\n", block_name);
                ret = -EINVAL;
                goto err_parser;
            }

            ect_list[j].block_precedence = i;
        }
    }

    ect_override_g3d_tables();
    ect_override_g3d_pll_table();

    ect_header_info.block_handle = ect_header;

    ect_print_all_blocks_once();

    return ret;

err_parser:
err_parse_string:
err_memcmp:
    kfree(ect_header);

    return ret;
}

int ect_strcmp(char *src1, char *src2) {
    for (; *src1 == *src2; src1++, src2++)
        if (*src1 == '\0')
            return 0;

    return ((*(unsigned char *)src1 < *(unsigned char *)src2) ? -1 : +1);
}

int ect_strncmp(char *src1, char *src2, int length) {
    int i;

    if (length <= 0)
        return -1;

    for (i = 0; i < length; i++, src1++, src2++)
        if (*src1 != *src2)
            return ((*(unsigned char *)src1 < *(unsigned char *)src2) ? -1
                                                                      : +1);

    return 0;
}

void ect_init_map_io(void) {
    int page_size, i;
    struct page *page;
    struct page **pages;
    int ret;

    if (!ect_early_vm.phys_addr || !ect_early_vm.size) {
        pr_info("[ECT] : skip mapping because early vm is not initialized\n");
        return;
    }

    page_size = ect_early_vm.size / PAGE_SIZE;
    if (ect_early_vm.size % PAGE_SIZE)
        page_size++;
    pages = kzalloc((sizeof(struct page *) * page_size), GFP_KERNEL);
    page = phys_to_page(ect_early_vm.phys_addr);

    for (i = 0; i < page_size; ++i)
        pages[i] = page++;

    ret = map_vm_area(&ect_early_vm, PAGE_KERNEL, pages);
    if (ret)
        pr_err("[ECT] : failed to mapping va and pa(%d)\n", ret);
    kfree(pages);
}
