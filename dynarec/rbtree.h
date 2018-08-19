/* Red-Black Tree implementation */

#ifndef __RBTREE_H_
#define __RBTREE_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum rbt_color {
   RBT_RED,
   RBT_BLACK
};

struct rbt_node {
   /* Node value */
   uint32_t key;
   /* Left child node or NULL */
   struct rbt_node *left;
   /* Right child node or NULL */
   struct rbt_node *right;
   /* Parent node or NULL if root */
   struct rbt_node *parent;
   /* Node color */
   enum rbt_color color;
};

struct rbtree {
   struct rbt_node *root;
};

void rbt_init(struct rbtree *t);

/* Insert `n` in `t`. If a node with the same key exists it's removed
   from the tree and returned, otherwise NULL is returned. */
struct rbt_node *rbt_insert(struct rbtree *t, struct rbt_node *n);
/* Search for a node matching `key` in the tree and return it. If no node is found returns
   NULL. */
struct rbt_node *rbt_find(struct rbtree *t, uint32_t key);
/* Visit the entire tree (in order from lowest to highest value) and
   run `visitor` on each node */
void rbt_visit(struct rbtree *t, void (*visitor)(struct rbt_node *));

#ifdef __cplusplus
}
#endif

#endif /* __RBTREE_H_ */
