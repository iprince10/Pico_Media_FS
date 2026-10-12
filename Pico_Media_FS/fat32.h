#ifndef fat32_h
#define fat32_h

uint8_t sd_mbr_parse(uint8_t *mbr, uint32_t *part_start, uint32_t *part_size);
// FAT32 geometry, everything the rest of the code needs to do addressing.
// part_start/fat_start/data_start are absolute blocks (disk wide), the rest
// are the raw BPB fields.
typedef struct
{
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t num_fats;
    uint32_t sectors_per_fat;
    uint32_t total_sectors;
    uint32_t root_cluster;
    uint32_t part_start;
    uint32_t fat_start;
    uint32_t data_start;
    uint32_t cluster_count;
} fat_geom_t;

uint8_t fat32_parse_bpb(uint8_t *bpb, uint32_t part_start, fat_geom_t *g);

uint8_t fat32_list_root(const fat_geom_t *g);

// uint8_t fat32_read_file(const fat_geom_t *g, uint32_t start, uint32_t size, fat32_sink_t sink, void *ctx);

void read_prince(const fat_geom_t *g);
void draw_prince(const fat_geom_t *g);

#endif