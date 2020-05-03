#include "include/light.h"

// TODO: there is currently a sunlight bug where sunlight which is propagating
// down in a column which is not its original column still maintains its value
// as it goes lower - this is something optional to fix tha probably (?) should
// be fixed.

#define QUEUE_SIZE 65536 

#define ENQUEUE(q, e)\
    assert(((q)->size + 1) < QUEUE_SIZE);\
    ((q)->elements[(q)->size++] = (e));

#define DEQUEUE(q) ((q)->elements[--(q)->size])

// max value for any light field
#define LIGHT_MAX 15

struct LightNode {
    ivec3s pos;
    u32 value;
};

struct LightQueue {
    struct LightNode elements[QUEUE_SIZE];
    size_t size;
};

enum PropagationType {
    DEFAULT_LIGHT, SUNLIGHT
};

static void add_propagate(
    struct World *world, struct LightQueue *queue,
    u32 mask, u32 offset, enum PropagationType type) {
    while (queue->size != 0) {
        struct LightNode node = DEQUEUE(queue);

        u32 light = world_get_all_light(world, node.pos),
            val = (light & mask) >> offset;

        // propagate in reverse of enum Direction order so DOWN is first
        // this will improve sunlight propagation speed
        for (enum Direction d = DOWN; (s32) d >= 0; d--) {
            bool sunlight_down = type == SUNLIGHT && d == DOWN; 

            ivec3s n_pos = glms_ivec3_add(node.pos, DIR2IVEC3S(d));
            u64 n_data = world_get_data(world, n_pos);
            u32 n_light = chunk_data_to_all_light(n_data),
                n_val = ((n_light & mask) >> offset);
            struct Block n_block = BLOCKS[chunk_data_to_block(n_data)];

            // TODO: it may be incorrect to check n_val != 0 to subvert the block behavior call
            if ((n_val != 0 || n_block.is_transparent(world, n_pos)) &&
                ((sunlight_down && n_val < val) || (n_val + 1 < val))) {

                // sunlight does not dim as it is propagated down
                u32 delta = sunlight_down ? 0 : -1;

                world_set_all_light(
                    world, n_pos,
                    (n_light & ~mask) | ((((light & mask) >> offset) + delta) << offset)
                );
                ENQUEUE(queue, ((struct LightNode) { .pos = n_pos }));
            }
        }
    }
}

static void remove_propagate(
    struct World *world, struct LightQueue *queue, struct LightQueue *prop_queue,
    u32 mask, u32 offset, enum PropagationType type) {
    while (queue->size != 0) {
        struct LightNode node = DEQUEUE(queue);
        u32 value = node.value;

        for (enum Direction d = 0; d < 6; d++) {
            ivec3s n_pos = glms_ivec3_add(node.pos, DIR2IVEC3S(d));
            u32 n_light = world_get_all_light(world, n_pos),
                n_value = (n_light & mask) >> offset;

            if ((n_light & mask) != 0 &&
                (n_value < value || (type == SUNLIGHT && d == DOWN))) {
                world_set_all_light(world, n_pos, n_light & ~mask);
                ENQUEUE(queue, ((struct LightNode) { .pos = n_pos, .value = n_value }));
            } else if (n_value >= value) {
                ENQUEUE(prop_queue, ((struct LightNode) { .pos = n_pos }));
            }
        }
    }
}

static void add_channel(
    struct World *world, ivec3s pos,
    u8 value, u32 mask, u32 offset, enum PropagationType type) {
    struct LightQueue queue = { .size = 0 };
    world_set_all_light(world, pos, (world_get_all_light(world, pos) & ~mask) | (((u32) value) << offset));
    ENQUEUE(&queue, ((struct LightNode) { .pos = pos }));
    add_propagate(world, &queue, mask, offset, type);
}

static void remove_channel(
    struct World *world, ivec3s pos,
    u32 mask, u32 offset, enum PropagationType type) {
    struct LightQueue
        queue = { .size = 0 },
        prop_queue = { .size = 0 };

    u32 light = world_get_all_light(world, pos); 
    world_set_all_light(world, pos, light & ~mask);

    ENQUEUE(&queue, ((struct LightNode) { .pos = pos, .value = (light & mask) >> offset }));
    remove_propagate(world, &queue, &prop_queue, mask, offset, type);
    add_propagate(world, &prop_queue, mask, offset, type);
}

void light_add(struct World *world, ivec3s pos, u16 light) {
    if (!BLOCKS[world_get_block(world, pos)].is_transparent(world, pos)) {
        return;
    }

    for (size_t i = 0; i < 4; i++) {
        u32 mask = 0xF << (i * 4), offset = i * 4;
        add_channel(world, pos, (light & mask) >> offset, mask, offset, DEFAULT_LIGHT);
    }
}

void light_remove(struct World *world, ivec3s pos) {
    for (size_t i = 0; i < 4; i++) {
        remove_channel(world, pos, 0xF << (i * 4), i * 4, DEFAULT_LIGHT);
    }
}

// updates lighting at the specified position, propagating light around it
void all_light_update(struct World *world, ivec3s pos) {
    struct LightQueue queue = { .size = 0 }; 

    // i is [0..4] instead of [0..3] so sunlight is propagated as well
    for (size_t i = 0; i < 5; i++) {
        u32 mask = 0xF << (i * 4), offset = i * 4;
        bool sunlight = i == 4;
        queue.size = 0;

        for (enum Direction d = 0; d < 6; d++) {
            if (!BLOCKS[world_get_block(world, pos)].is_transparent(world, pos)) {
                return;
            }

            ENQUEUE(&queue, ((struct LightNode) { .pos = glms_ivec3_add(pos, DIR2IVEC3S(d)) }));
        }

        // enqueue for sunlight if this block is above the heightmap
        if (sunlight &&
            pos.y > world_heightmap_get(world, (ivec2s) {{ pos.x, pos.z }})) {
            world_set_sunlight(world, pos, LIGHT_MAX);
            ENQUEUE(&queue, ((struct LightNode) { .pos = pos }));
            printf("doin it\n");
        }

        add_propagate(world, &queue, mask, offset, sunlight ? SUNLIGHT : DEFAULT_LIGHT);
    }
}

void all_light_remove(struct World *world, ivec3s pos) {
    light_remove(world, pos);
    remove_channel(world, pos, 0xF0000, 16, SUNLIGHT);
}

void all_light_apply(struct Chunk *chunk) {
    struct Heightmap *heightmap = chunk_get_heightmap(chunk);
    struct LightQueue queue = { .size = 0 };

    // propagate sunlight for this chunk
    for (s64 x = 0; x < CHUNK_SIZE.x; x++) {
        for (s64 z = 0; z < CHUNK_SIZE.z; z++) {
            s64 c_h = HEIGHTMAP_GET(heightmap, ((ivec2s) {{ x, z }})) - chunk->position.y;
            
            if (c_h < 0) {
                continue;
            }

            for (s64 y = CHUNK_SIZE.y - 1; y >= c_h; y--) {
                s64 y_w = chunk->position.y + y;
                ivec3s pos_c = (ivec3s) {{ x, y, z }},
                    pos_w = glms_ivec3_add(chunk->position, pos_c);

                chunk_set_sunlight(chunk, pos_c, LIGHT_MAX);

                // check if this sunlight needs to be propagated in any
                // N, E, S, W direction before queueing it
                for (enum Direction d = NORTH; d <= WEST; d++) {
                    ivec3s
                        d_v = DIR2IVEC3S(d),
                        pos_cn = glms_ivec3_add(pos_c, d_v),
                        pos_wn = glms_ivec3_add(pos_w, d_v);

                    if (y_w < (chunk_in_bounds(pos_cn) ?
                            HEIGHTMAP_GET(heightmap, ((ivec2s) {{ pos_cn.x, pos_cn.z }})) :
                            world_heightmap_get(chunk->world, ((ivec2s) {{ pos_wn.x, pos_wn.z }})))) {
                        ENQUEUE(&queue, ((struct LightNode) { .pos = pos_w }));
                        break;
                    }
                }
            }
        }
    }

    add_propagate(chunk->world, &queue, 0xF0000, 16, SUNLIGHT);

    // fill queue_border with all positions of all non-empty light values on
    // the immediate eborder of this chunk
    struct LightQueue queue_border = { .size = 0 };
    ivec3s pos;
    u32 light;

    for (s64 x = 0; x < CHUNK_SIZE.x; x++) {
        for (s64 z = 0; z < CHUNK_SIZE.z; z++) {
            pos = glms_ivec3_add(chunk->position, (ivec3s) {{ x, -1, z }});
            light = world_get_all_light(chunk->world, pos);

            if (light != 0) {
                ENQUEUE(&queue_border, ((struct LightNode) { .pos = pos, .value = light }));
            }

            pos = glms_ivec3_add(chunk->position, (ivec3s) {{ x, CHUNK_SIZE.y, z }});
            light = world_get_all_light(chunk->world, pos);

             if (light != 0) {
                ENQUEUE(&queue_border, ((struct LightNode) { .pos = pos, .value = light }));
            }
        }
    }

    for (s64 x = 0; x < CHUNK_SIZE.x; x++) {
        for (s64 y = 0; y < CHUNK_SIZE.y; y++) {
            pos = glms_ivec3_add(chunk->position, (ivec3s) {{ x, y, -1 }});
            light = world_get_all_light(chunk->world, pos);

            if (light != 0) {
                ENQUEUE(&queue_border, ((struct LightNode) { .pos = pos, .value = light }));
            }

            pos = glms_ivec3_add(chunk->position, (ivec3s) {{ x, y, CHUNK_SIZE.z }});
            light = world_get_all_light(chunk->world, pos);

            if (light != 0) {
                ENQUEUE(&queue_border, ((struct LightNode) { .pos = pos, .value = light }));
            }
        }
    }

    for (s64 y = 0; y < CHUNK_SIZE.y; y++) {
        for (s64 z = 0; z < CHUNK_SIZE.z; z++) {
            pos = glms_ivec3_add(chunk->position, (ivec3s) {{ -1, y, z }});
            light = world_get_all_light(chunk->world, pos);

            if (light != 0) {
                ENQUEUE(&queue_border, ((struct LightNode) { .pos = pos, .value = light }));
            }

            pos = glms_ivec3_add(chunk->position, (ivec3s) {{ CHUNK_SIZE.x, y, z }});
            light = world_get_all_light(chunk->world, pos);

            if (light != 0) {
                ENQUEUE(&queue_border, ((struct LightNode) { .pos = pos, .value = light }));
            }
        }
    }

    // propagate each channel if the channel's value in the border position is
    // nonzero
    for (size_t i = 0; i < 4; i++) {
        u32 mask = 0xF << (i * 4), offset = i * 4;
        queue.size = 0;

        for (size_t j = 0; j < queue_border.size; j++) {
            struct LightNode n = queue_border.elements[i];
            if ((n.value & mask) != 0) {
                ENQUEUE(&queue, ((struct LightNode) { .pos = n.pos }));
            }
        }

        add_propagate(chunk->world, &queue, mask, offset, DEFAULT_LIGHT);
    }
}

// removes sunlight at the specified position
void sunlight_remove(struct World *world, ivec3s pos) {
    if (!BLOCKS[world_get_block(world, pos)].is_transparent(world, pos)) {
        return;
    }

    remove_channel(world, pos, 0xF0000, 16, SUNLIGHT);
}