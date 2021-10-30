#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "bcachefs.h"

// Our data structure structs are really just header of contiguous lists.  Most
// of the time, the header always start with the size of full list in bytes
//
// The elements are starting after the header struct, they can have different
// size, their size is also stored in the first few bytes of the element u64s
// can have different sizes (uint8_t to uint64_t), to know the number of bytes
// used to store the size we need to use the `struct u64s_spec`
//
//  * struct parent_header {
//  |  u64s		// Size of the entire data structure in `BCH_U64S_SIZE`
//  |  metadata
//  |     .
//  |     .
//  |     .
//  |  metadata
//  | };
//  | *  struct child {
//  | |   u64s          // Size of this value in `BCH_U64S_SIZE`
//  | +> };
//  | *  struct child {
//  | |   u64s          // Size of this value in `BCH_U64S_SIZE`
//  | +> };
//  +>                  // End of data structure
//
//  end         = &parent_header + header->u64s;
//  first_value = &parent_header + sizeof(header);


// Reads the u64s field contained inside a struct (assume it is the first
// field) note that fields like `uint64_t _data[0];` do not contribute to the
// struct size and create a pointer to the begining of the struct.
//
// TODO: duplicate with benz_uintXX_as_uint64
uint64_t read_u64s(const void *c, struct u64s_spec u64s_spec) {
    uint64_t u64s = 0;
    switch (u64s_spec.size)
    {
    case sizeof(uint8_t):
        u64s = *((const uint8_t*)c);
        break;
    case sizeof(uint16_t):
        u64s = *((const uint16_t*)c);
        break;
    case sizeof(uint32_t):
        u64s = *((const uint32_t*)c);
        break;
    case sizeof(uint64_t):
        u64s = *((const uint64_t*)c);
        break;
    default:
        u64s = 0;
    }
    return u64s;
}

// Gets next element, reads the size of the current element and jump to the
// next one
//
// if current element is null, set it to the first element
// if current element is higher than the end returns null
//
// p            : start of our parent data structure
// size_of_p    : size of the header of our parent data structure
// p_end        : end of the elements
// c            : current child element
// u64s_spec    : number of bytes used to store the element size
const void *benz_bch_next_sibling(const void *p, uint32_t sizeof_p, const void *p_end, const void *c, struct u64s_spec u64s_spec)
{
    if (c == NULL)
    {
        // if null fetch first element which is right after the header
        c = (const uint8_t*)p + sizeof_p;
    }
    else
    {
        // fetch next element by reading the size of current element and jumping to the
        // next one
        uint64_t u64s = read_u64s(c, u64s_spec) + u64s_spec.start;
        c = (const uint8_t*)c + u64s * BCH_U64S_SIZE;
    }

    // if we reached the end simply return null
    if (c >= p_end)
    {
        c = NULL;
    }
    return c;
}

// Iterate through superblock field looking for a specific field type. If `type
// == BCH_SB_FIELD_NR` then next field is returned
const struct bch_sb_field *benz_bch_next_sb_field(const struct bch_sb *p, const struct bch_sb_field *c, enum bch_sb_field_type type)
{
    const uint8_t *p_end = (const uint8_t*)p + p->u64s * BCH_U64S_SIZE;
    do
    {
        c = (const struct bch_sb_field*)benz_bch_next_sibling(p, sizeof(*p), p_end, c, U64S_BCH_SB_FIELD);
    } while (c && type != BCH_SB_FIELD_NR && c->type != type);
    return c;
}

// Iterate through journal set entries looking for a specific field type. If
// `type == BCH_JSET_ENTRY_NR` then next entry is returned
const struct jset_entry *benz_bch_next_jset_entry(const struct bch_sb_field *p,
                                                  uint32_t sizeof_p,
                                                  const struct jset_entry *c,
                                                  enum bch_jset_entry_type type)
{
    const uint8_t *p_end = (const uint8_t*)p + p->u64s * BCH_U64S_SIZE;
    do
    {
        c = (const struct jset_entry*)benz_bch_next_sibling(p, sizeof_p, p_end, c, U64S_JSET_ENTRY);
    } while (c && type != BCH_JSET_ENTRY_NR && c->type != type);
    return c;
}

// Returns the first value held by a bkey
const struct bch_val *benz_bch_first_bch_val(const struct bkey *p, uint8_t key_u64s)
{
    const struct bch_val *p_end = (const void*)((const uint8_t*)p + p->u64s * BCH_U64S_SIZE);
    const struct bch_val *c = (const void*)((const uint8_t*)p + key_u64s * BCH_U64S_SIZE);
    if (c >= p_end)
    {
        c = NULL;
    }
    return c;
}

// This is actually returning next btree pointer when we already have one
//
// p       : is our initial parent entry
// c       : is our child
// sizeof_c: is the size of child
const struct bch_val *benz_bch_next_bch_val(const struct bkey *p, const struct bch_val *c, uint32_t sizeof_c)
{
    const struct bch_val *p_end = (const void*)((const uint8_t*)p + p->u64s * BCH_U64S_SIZE);
    c = (const void*)((const uint8_t*)c + sizeof_c);
    if (c >= p_end)
    {
        c = NULL;
    }
    return c;
}

// Fetch next valid bset
const struct bset *benz_bch_next_bset(const struct btree_node *p, const void *p_end, const struct bset *c, const struct bch_sb *sb)
{
    uint64_t block_size = benz_bch_get_block_size(sb);
    do
    {
        if (c == NULL)
        {
            c = &p->keys;
        }
        else
        {
            // We want to find the next bset which is located at the next
            // block_size from the beginning of parent. It is possible for
            // `(uint64_t)p % block_size == 0` to always be true but in case it
            // could not be, reposition _cb to be relative to the beginning of
            // p when looking for the next block_size location, then move back
            // to the correct location in RAM
            const uint8_t *_cb = (const uint8_t*)c;
            _cb -= (uint64_t)p;

            // next bset
            _cb += sizeof(*c) + c->u64s * BCH_U64S_SIZE;

            // bset starts at a blocksize
            _cb += block_size - (uint64_t)_cb % block_size +
                   // skip btree_node_entry csum
                   sizeof(struct bch_csum);

            _cb += (uint64_t)p;
            c = (const void*)_cb;
        }
        if ((const void*)c >= p_end)
        {
            c = NULL;
        }
    } while (c && !c->u64s);
    return c;
}

// Iterate through bkeys inside a bset looking for a specific key type if `type
// == KEY_TYPE_MAX` then next key is returned
const struct bkey *benz_bch_next_bkey(const struct bset *p, const struct bkey *c, enum bch_bkey_type type)
{
    const uint8_t *p_end = (const uint8_t*)p->start + p->u64s * BCH_U64S_SIZE;
    do
    {
        c = (const struct bkey*)benz_bch_next_sibling(p, sizeof(*p), p_end, c, U64S_BKEY);
    } while (c && type != KEY_TYPE_MAX && c->type != type);
    return c;
}

struct bkey_local benz_bch_parse_bkey(const struct bkey *bkey, const struct bkey_format *format)
{
    struct bkey_local ret = {.u64s = bkey->u64s,
                             .format = bkey->format,
                             .needs_whiteout = bkey->needs_whiteout,
                             .type = bkey->type};
    if (bkey->format == KEY_FORMAT_LOCAL_BTREE &&
            memcmp(format, &BKEY_FORMAT_SHORT, sizeof(struct bkey_format)) == 0)
    {
        const struct bkey_short *bkey_short = (const void*)bkey;
        ret.p = bkey_short->p;
        ret.key_u64s = format->key_u64s;
    }
    else if (bkey->format == KEY_FORMAT_LOCAL_BTREE)
    {
        const uint8_t *bytes = (const void*)bkey;
        bytes += format->key_u64s * BCH_U64S_SIZE;
        for (int i = 0; i < BKEY_NR_FIELDS ; ++i)
        {
            uint64_t value = format->field_offset[i];
            if (value + format->bits_per_field[i] == 0)
            {
                continue;
            }
            bytes -= format->bits_per_field[i] / 8;
            if (format->bits_per_field[i])
            {
                value += benz_uintXX_as_uint64(bytes, format->bits_per_field[i]);
            }
            switch (i)
            {
            case BKEY_FIELD_INODE:
                ret.p.inode = value;
                break;
            case BKEY_FIELD_OFFSET:
                ret.p.offset = value;
                break;
            case BKEY_FIELD_SNAPSHOT:
                ret.p.snapshot = (uint32_t)value;
                break;
            case BKEY_FIELD_SIZE:
                ret.size = (uint32_t)value;
                break;
            case BKEY_FIELD_VERSION_HI:
                ret.version.hi = (uint32_t)value;
                break;
            case BKEY_FIELD_VERSION_LO:
                ret.version.lo = value;
                break;
            }
        }
        ret.key_u64s = format->key_u64s;
    }
    else if (bkey->format == KEY_FORMAT_CURRENT)
    {
        memcpy(&ret, bkey, sizeof(*bkey));
        ret.key_u64s = BKEY_U64s;
    }
    return ret;
}

#pragma GCC push_options
#pragma GCC optimize ("O2")

static uint32_t benz_getle32(const void* p, int64_t off){
    uint32_t x;
    memcpy(&x, (char*)p+off, sizeof(x));
    return x;
}

static uint64_t benz_getle64(const void* p, int64_t off){
    uint64_t x;
    memcpy(&x, (char*)p+off, sizeof(x));
    return x;
}

#define luai_unlikely(x) x

unsigned benz_ctz64(uint64_t x){
    return x ? __builtin_ctzll(x) : 64;
}

int   benz_bch_inode_unpack_size(uint64_t*               bi_size,
                                 const struct bch_inode* p,
                                 const void*             end){
    register int      new_varint, nr_fields;
    register int      varintc;
    register uint32_t bi_flags;
    register uint64_t f;
    const uint8_t* e = (const uint8_t*)end;
    const uint8_t* r = (const uint8_t*)&p->fields;

    *bi_size = 0;/* Default is 0. */
    if(e<r)
        return -1;/* Parse error, end pointer behind field pointer! */

    bi_flags   = benz_getle32(&p->bi_flags, 0);
    nr_fields  = (int)(bi_flags >> 24) & 127;
    new_varint = !!(bi_flags & BCH_INODE_FLAG_new_varint);

    if(!new_varint)
        return -2;/* Parse error, old-style varint! */

    if(e-r < (ptrdiff_t)nr_fields)
        return -3;/* Parse error, end pointer far too short! At least 1 byte/field. */

    /**
     * The field bi_size is the 5th field and 9th varint in a v2-packed inode,
     * being preceded by four wide (double-varint) fields (the 96-bit timestamps).
     *
     * Accordingly, check that the number of fields is at least 5, and if so
     * then scan up to the 9th varint.
     */

    if(nr_fields < 5)
        return  -4;/* No size field encoded, default is 0. */

    for(varintc=0; varintc<9; varintc++){
        f  = benz_ctz64(*r+1)+1;
        r += f;
        if(luai_unlikely(r>e))
            return -5;
    }

    /**
     * Pointer r now points one byte past the end of the target varint.
     * Decode varint at current location.
     */

    f *= 6;
    f &= 0x3F;/* Can be elided on x86_64 */
    /* For field length:   9  8  7  6  5  4  3  2  1    */
    /* Shift right by: --  0  8 15 22 29 36 43 50 57 -- */
    f  = 000101726354453627100 >> f;
    f &= 0x3F;/* Can be elided on x86_64 */
    f  = benz_getle64(r,-8) >> f;
    *bi_size = f;

    return 0;
}
#pragma GCC pop_options

inline uint64_t benz_bch_get_block_size(const struct bch_sb *sb)
{
    return (uint64_t)sb->block_size * BCH_SECTOR_SIZE;
}

inline uint64_t benz_bch_get_btree_node_size(const struct bch_sb *sb)
{
    return (uint64_t)(uint16_t)benz_get_flag_bits(sb->flags[0], 12, 28) * BCH_SECTOR_SIZE;
}

inline uint64_t benz_bch_get_extent_offset(const struct bch_extent_ptr *bch_extent_ptr)
{
    return bch_extent_ptr->offset * BCH_SECTOR_SIZE;
}

const struct bkey *benz_bch_file_offset_size(const struct bkey *bkey,
                                             const struct bch_val *bch_val,
                                             uint64_t *file_offset,
                                             uint64_t *offset,
                                             uint64_t *size)
{
    if (bch_val && bkey->type == KEY_TYPE_extent)
    {
        *file_offset = (bkey->p.offset - bkey->size) * BCH_SECTOR_SIZE;
        *offset = benz_bch_get_extent_offset((const struct bch_extent_ptr*)bch_val);
        *size = bkey->size * BCH_SECTOR_SIZE;
    }
    else if (bch_val && bkey->type == KEY_TYPE_inline_data)
    {
        *file_offset = (bkey->p.offset - bkey->size) * BCH_SECTOR_SIZE;
        *offset = 0;
        *size = bkey->u64s * BCH_U64S_SIZE;
    }
    else
    {
        bkey = NULL;
    }
    return bkey;
}

uint64_t benz_bch_inline_data_offset(const struct btree_node* start, const struct bch_val *bch_val, uint64_t start_offset)
{
    return (uint64_t)((const uint8_t*)bch_val - (const uint8_t*)start) + start_offset;
}

// Get the superblock size.
uint64_t benz_bch_get_sb_size(const struct bch_sb *sb){
    return offsetof(struct bch_sb, _data) + sb->u64s*BCH_U64S_SIZE;
}

struct bch_sb *benz_bch_realloc_sb(struct bch_sb *sb, uint64_t size)
{
    return realloc(sb, size);
}

inline struct btree_node *benz_bch_malloc_btree_node(const struct bch_sb *sb)
{
    return malloc(benz_bch_get_btree_node_size(sb));
}

uint64_t benz_bch_fread_sb(struct bch_sb *sb, uint64_t size, FILE *fp)
{
    return fseek(fp, BCH_SB_SECTOR * BCH_SECTOR_SIZE, SEEK_SET) == 0 &&
           fread(sb, size, 1, fp) == 1;
}

uint64_t benz_bch_fread_btree_node(struct btree_node *btree_node, const struct bch_sb *sb, const struct bch_btree_ptr_v2 *btree_ptr, FILE *fp)
{
    uint64_t offset = benz_bch_get_extent_offset(btree_ptr->start);
    fseek(fp, (long)offset, SEEK_SET);
    memset(btree_node, 0, benz_bch_get_btree_node_size(sb));
    return fread(btree_node, btree_ptr->sectors_written * BCH_SECTOR_SIZE, 1, fp) == 1;
}

// Filesystem and iterator abstraction layer
// -----------------------------------------
int Bcachefs_early_probe_size(Bcachefs *this)
{
    struct uuid magic;
    uint32_t    u64s;
    const long  sb_offset    = BCH_SB_SECTOR*BCH_SECTOR_SIZE;
    const long  magic_offset = offsetof(struct bch_sb, magic);
    const long  u64s_offset  = offsetof(struct bch_sb, u64s);
    
    
    /* Seek to bch_sb->magic, read and verify */
    if(fseek(this->fp, sb_offset+magic_offset, SEEK_SET) != 0)
        return 1;
    if(fread(&magic, sizeof(magic), 1, this->fp) != 1)
        return 1;
    if(memcmp(&magic, &BCACHE_MAGIC, sizeof(magic)) != 0)
        return 1;
    
    
    /* Seek to bch_sb->u64s and read */
    if(fseek(this->fp, sb_offset+u64s_offset, SEEK_SET) != 0)
        return 1;
    if(fread(&u64s, sizeof(u64s), 1, this->fp) != 1)
        return 1;
    this->sb_size = offsetof(struct bch_sb, _data) + BCH_U64S_SIZE*benz_getle32(&u64s,0);
    
    
    /* Seek to end to measure length */
    if(fseek(this->fp, 0, SEEK_END) != 0)
        return 1;
    this->size = ftell(this->fp);
    if(fseek(this->fp, 0, SEEK_SET) != 0)
        return 1;
    
    
    /* Return */
    return 0;
}

int Bcachefs_fini(Bcachefs *this)
{
    return Bcachefs_close(this);
}

int Bcachefs_open(Bcachefs *this, const char *path)
{
    *this = (Bcachefs){0};
    this->fp = fopen(path, "rb");
    if(!this->fp)
        goto error;

    if(Bcachefs_early_probe_size(this) != 0)
        goto error;

    this->sb = benz_bch_realloc_sb(NULL, this->sb_size);
    if(!this->sb)
        goto error;

    if(!benz_bch_fread_sb(this->sb, this->sb_size, this->fp))
        goto error;
    
    
    return 1;
    
    
    error:
    Bcachefs_fini(this);
    return 0;
}

int Bcachefs_close(Bcachefs *this)
{
    if(this->fp)
        fclose(this->fp);
    
    if(this->sb)
        free(this->sb);
    
    this->fp = NULL;
    this->sb = NULL;
    return 1;
}

int Bcachefs_iter(const Bcachefs *this, Bcachefs_iterator *iter, enum btree_id type)
{
    *iter = (Bcachefs_iterator){0};

    iter->type = type;
    iter->btree_node = benz_bch_malloc_btree_node(this->sb);
    iter->jset_entry = Bcachefs_iter_next_jset_entry(this, iter);
    if(!iter->jset_entry)
        return 0;/* No B-tree with that BTREE_ID. */
    
    iter->btree_ptr = Bcachefs_iter_next_btree_ptr(this, iter);
    if (iter->btree_ptr && !benz_bch_fread_btree_node(iter->btree_node,
                                                      this->sb,
                                                      iter->btree_ptr,
                                                      this->fp))
    {
        iter->btree_ptr = NULL;
    }
    return iter->jset_entry && iter->btree_node && iter->btree_ptr;
}

int Bcachefs_next_iter(const Bcachefs *this, Bcachefs_iterator *iter, const struct bch_btree_ptr_v2 *btree_ptr)
{
    Bcachefs_iterator *next_it = malloc(sizeof(Bcachefs_iterator));

    *next_it = (Bcachefs_iterator){
        .type = iter->type,
        .btree_node = benz_bch_malloc_btree_node(this->sb),
        .btree_ptr = btree_ptr
    };

    if (next_it->btree_ptr && !benz_bch_fread_btree_node(next_it->btree_node,
                                                         this->sb,
                                                         next_it->btree_ptr,
                                                         this->fp))
    {
        next_it->btree_ptr = NULL;
    }

    if (next_it->btree_node && next_it->btree_ptr)
    {
        iter->next_it = next_it;
        return 1;
    }
    else
    {
        Bcachefs_iter_fini(this, next_it);
        free(next_it);
        next_it = NULL;
        return 0;
    }
}

int Bcachefs_iter_fini(const Bcachefs *this, Bcachefs_iterator *iter)
{
    (void)this;
    if (iter == NULL)
    {
        return 1;
    }
    if (iter->next_it && Bcachefs_iter_fini(this, iter->next_it))
    {
        free(iter->next_it);
        iter->next_it = NULL;
    }
    if (iter->btree_node)
    {
        free(iter->btree_node);
        iter->btree_node = NULL;
    }
    *iter = (Bcachefs_iterator){
        .type = BTREE_ID_NR,
        .btree_node = iter->btree_node,
        .next_it = iter->next_it
    };
    return iter->next_it == NULL && iter->btree_node == NULL;
}

const struct bch_val *_Bcachefs_iter_next_bch_val(const struct bkey *bkey, const struct bkey_format* format)
{
    uint8_t key_u64s = 0;
    if (bkey == NULL)
    {
        return NULL;
    }
    if (bkey->format == KEY_FORMAT_LOCAL_BTREE)
    {
        key_u64s = format->key_u64s;
    }
    else
    {
        key_u64s = BKEY_U64s;
    }
    return benz_bch_first_bch_val(bkey, key_u64s);
}

const struct bch_val *Bcachefs_iter_next(const Bcachefs *this, Bcachefs_iterator *iter)
{
    const struct bkey *bkey = NULL;
    const struct bch_val *bch_val = NULL;

    // Wind to current iterator
    if (iter->next_it)
    {
        bch_val = Bcachefs_iter_next(this, iter->next_it);
        if (bch_val)
        {
            return bch_val;
        }
        else
        {
            Bcachefs_iter_fini(this, iter->next_it);
            free(iter->next_it);
            iter->next_it = NULL;
        }
    }
    if (iter->bset == NULL && iter->btree_ptr)
    {
        iter->bset = Bcachefs_iter_next_bset(this, iter);
    }
    if (iter->btree_ptr && iter->bset) {}
    else
    {
        return NULL;
    }
    do
    {
        iter->bkey = benz_bch_next_bkey(iter->bset, iter->bkey, KEY_TYPE_MAX);
        bch_val = _Bcachefs_iter_next_bch_val(iter->bkey, &iter->btree_node->format);
    } while (iter->bkey && bch_val == NULL);
    bkey = iter->bkey;
    switch ((int)iter->type)
    {
    case BTREE_ID_extents:
    case BTREE_ID_inodes:
    case BTREE_ID_dirents:
        iter->bch_val = bch_val;
        if (bch_val && bkey->type == KEY_TYPE_btree_ptr_v2 &&
                Bcachefs_next_iter(this, iter, (const struct bch_btree_ptr_v2*)bch_val))
        {
            return Bcachefs_iter_next(this, iter);
        }
        else if (bch_val)
        {
            return bch_val;
        }
        break;
    default:
        return NULL;
    }
    if (iter->bkey == NULL)
    {
        iter->bset = Bcachefs_iter_next_bset(this, iter);
    }
    if (iter->bset == NULL)
    {
        iter->btree_ptr = NULL;
    }
    return Bcachefs_iter_next(this, iter);
}

const struct jset_entry *Bcachefs_iter_next_jset_entry(const Bcachefs *this, Bcachefs_iterator *iter)
{
    const struct jset_entry *jset_entry = iter->jset_entry;
    const struct bch_sb_field *sb_field_clean = (const void*)benz_bch_next_sb_field(
                this->sb,
                NULL,
                BCH_SB_FIELD_clean);

    // if sb_field_clean == NULL then the archive needs to be fsck
    // TODO: we need to return an error all the way back to python here
    assert(sb_field_clean != NULL);
    jset_entry = benz_bch_next_jset_entry(sb_field_clean,
                                          sizeof(struct bch_sb_field_clean),
                                          jset_entry,
                                          BCH_JSET_ENTRY_btree_root);

    assert(jset_entry != NULL);
    for (; jset_entry && jset_entry->btree_id != iter->type;
         jset_entry = benz_bch_next_jset_entry(sb_field_clean,
                                               sizeof(struct bch_sb_field_clean),
                                               jset_entry,
                                               BCH_JSET_ENTRY_btree_root)) {}
    return jset_entry;
}

const struct bch_btree_ptr_v2 *Bcachefs_iter_next_btree_ptr(const Bcachefs *this, Bcachefs_iterator *iter)
{
    (void)this;
    const struct jset_entry *jset_entry = iter->jset_entry;
    const struct bch_btree_ptr_v2 *btree_ptr = iter->btree_ptr;
    if (btree_ptr)
    {
        btree_ptr = (const void*)benz_bch_next_bch_val(&jset_entry->start->k,
                                                       (const void*)btree_ptr,
                                                       sizeof(struct bch_btree_ptr_v2));
    }
    else
    {
        btree_ptr = (const void*)benz_bch_first_bch_val(&jset_entry->start->k, BKEY_U64s);
    }
    const struct bch_val *bch_val = (const void*)btree_ptr;
    for (; bch_val && btree_ptr->start->unused;
         bch_val = benz_bch_next_bch_val(&jset_entry->start->k,
                                         bch_val,
                                         sizeof(struct bch_btree_ptr_v2)),
         btree_ptr = (const void*)bch_val) {}
    return btree_ptr;
}

const struct bset *Bcachefs_iter_next_bset(const Bcachefs *this, Bcachefs_iterator *iter)
{
    const struct btree_node *btree_node = iter->btree_node;
    const void *btree_node_end = (const uint8_t*)iter->btree_node + iter->btree_ptr->sectors_written * BCH_SECTOR_SIZE;
    const struct bset *bset = iter->bset;
    return benz_bch_next_bset(btree_node, btree_node_end, bset, this->sb);
}

Bcachefs_extent Bcachefs_iter_make_extent(const Bcachefs *this, Bcachefs_iterator *iter)
{
    (void)this;

    while (iter->next_it)
    {
        iter = iter->next_it;
    }

    const struct bkey_local bkey_local = benz_bch_parse_bkey(iter->bkey, &iter->btree_node->format);
    const struct bkey *bkey = (const void*)&bkey_local;
    Bcachefs_extent extent = {.inode = bkey->p.inode};
    benz_bch_file_offset_size(bkey, iter->bch_val, &extent.file_offset, &extent.offset, &extent.size);
    if (bkey->type == KEY_TYPE_inline_data)
    {
        extent.offset = benz_bch_inline_data_offset(iter->btree_node, iter->bch_val,
                                                    benz_bch_get_extent_offset(iter->btree_ptr->start));
        extent.size -= (uint64_t)((const uint8_t*)iter->bch_val - (const uint8_t*)iter->bkey);
    }
    return extent;
}

Bcachefs_inode Bcachefs_iter_make_inode(const Bcachefs *this, Bcachefs_iterator *iter)
{
    (void)this;

    while (iter->next_it)
    {
        iter = iter->next_it;
    }

    const struct bkey *bkey = iter->bkey;
    const struct bkey_local bkey_local = benz_bch_parse_bkey(bkey, &iter->btree_node->format);
    const struct bch_inode *bch_inode = (const void*)iter->bch_val;

    const void *p_end = (const void*)((const uint8_t*)bkey + bkey->u64s * BCH_U64S_SIZE);

    Bcachefs_inode inode = {0};
    inode.inode = bkey_local.p.offset;

    benz_bch_inode_unpack_size(&inode.size, bch_inode, p_end);
    return inode;
}

Bcachefs_dirent Bcachefs_iter_make_dirent(const Bcachefs *this, Bcachefs_iterator *iter)
{
    (void)this;

    while (iter->next_it)
    {
        iter = iter->next_it;
    }
    const struct bkey *bkey = iter->bkey;
    const struct bkey_local bkey_local = benz_bch_parse_bkey(bkey, &iter->btree_node->format);
    const struct bch_dirent *bch_dirent = (const void*)iter->bch_val;
    const uint8_t name_len = strlen((const void*)bch_dirent->d_name);
    const uint8_t max_name_len = (const uint8_t*)bkey + bkey->u64s * BCH_U64S_SIZE - bch_dirent->d_name;
    return (Bcachefs_dirent){.parent_inode = bkey_local.p.inode,
                                  .inode = bch_dirent->d_inum,
                                  .type = bch_dirent->d_type,
                                  .name = bch_dirent->d_name,
                                  .name_len = (name_len < max_name_len ? name_len : max_name_len)};
}

inline uint64_t benz_get_flag_bits(const uint64_t bitfield, uint8_t first_bit, uint8_t last_bit)
{
    return bitfield << (sizeof(bitfield) * 8 - last_bit) >> (sizeof(bitfield) * 8 - last_bit + first_bit);
}

uint64_t benz_uintXX_as_uint64(const uint8_t *bytes, uint8_t sizeof_uint)
{
    switch (sizeof_uint)
    {
    case 64:
        return *(const uint64_t*)(const void*)bytes;
    case 32:
        return *(const uint32_t*)(const void*)bytes;
    case 16:
        return *(const uint16_t*)(const void*)bytes;
    case 8:
        return *(const uint8_t*)bytes;
    }
    return (uint64_t)-1;
}

void benz_print_chars(const uint8_t* bytes, uint64_t len)
{
    for (uint64_t i = 0; i < len; ++i)
    {
        printf("%c", bytes[i]);
    }
}

void benz_print_bytes(const uint8_t* bytes, uint64_t len)
{
    for (uint64_t i = 0; i < len; ++i)
    {
        if (i && i % 4 == 0)
        {
            printf(" ");
        }
        if (i && i % 32 == 0)
        {
            printf("\n");
        }
        benz_print_hex(bytes + i, 1);
    }
}

void benz_print_bits(uint64_t bitfield)
{
    uint8_t* bytes = (uint8_t*)&bitfield;
    for (int i = 0, e = sizeof(bitfield) / sizeof(uint8_t); i < e; ++i)
    {
        for (int j = sizeof(uint8_t) * 8; j > 0; --j)
        {
            if (bytes[i] & 128)
            {
                printf("1");
            }
            else
            {
                printf("0");
            }
            bytes[i] <<= 1;
        }
        printf(" ");
    }
}

void benz_print_hex(const uint8_t *hex, uint64_t len)
{
    for (uint64_t i = 0; i < len; ++i)
    {
        printf("%02x", hex[i]);
    }
}

void benz_print_uuid(const struct uuid *uuid)
{
    unsigned int i = 0;
    benz_print_hex(&uuid->bytes[i], 4);
    i+=4;
    printf("-");
    benz_print_hex(&uuid->bytes[i], 2);
    i+=2;
    printf("-");
    benz_print_hex(&uuid->bytes[i], 2);
    i+=2;
    printf("-");
    benz_print_hex(&uuid->bytes[i], 2);
    i+=2;
    printf("-");
    benz_print_hex(&uuid->bytes[i], sizeof(*uuid) - i);
}
