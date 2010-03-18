/* Double linked list macros derived from include/linux/list.h. */


/* List head. */

struct list_head {
    struct list_head *next, *prev;
};

#define LIST_HEAD_INIT(name)  { &(name), &(name) }
#define LIST_HEAD(name)  struct list_head name = LIST_HEAD_INIT(name)

static inline void INIT_LIST_HEAD(struct list_head *list)
{
    list->next = list;
    list->prev = list;
}


/* Insert a new entry between two known consecutive entries. */
static inline void __list_add(
    struct list_head *new, struct list_head *prev, struct list_head *next)
{
    next->prev = new;
    new->next = next;
    new->prev = prev;
    prev->next = new;
}

/* Insert a new entry after the specified head. */
static inline void list_add(struct list_head *new, struct list_head *head)
{
    __list_add(new, head, head->next);
}


/* Insert a new entry before the specified head. */
static inline void list_add_tail(struct list_head *new, struct list_head *head)
{
    __list_add(new, head->prev, head);
}

/* Delete a list entry by making the prev/next entries
 * point to each other. */
static inline void __list_del(struct list_head * prev, struct list_head * next)
{
    next->prev = prev;
    prev->next = next;
}

/* Deletes entry from list. */
static inline void list_del(struct list_head *entry)
{
    __list_del(entry->prev, entry->next);
}


/* Casts a member of a structure out to the containing structure. */
#define container_of(ptr, type, member) \
    ( { \
        const typeof(((type *)0)->member) *__mptr = (ptr); \
        (type *)((char *)__mptr - offsetof(type, member)); \
    } )


/* Get the struct for this entry. */
#define list_entry(ptr, type, member)  container_of(ptr, type, member)

/* Iterates over list head, setting pos to each entry in the list. */
#define list_for_each(pos, head) \
    for (struct list_head *pos = (head)->next; pos != (head); pos = pos->next)


/* list_for_each_entry - iterate over list of given type
 *  type:       the type of the list
 *  member:     the name of the list_struct within the struct.
 *  pos:        the type * to use as a loop cursor.
 *  head:       the head for your list.
 */
#define list_for_each_entry(type, member, pos, head) \
    for (type *pos = list_entry((head)->next, type, member); \
    &pos->member != (head); \
    pos = list_entry(pos->member.next, type, member))
