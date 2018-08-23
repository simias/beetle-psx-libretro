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
   /* Left child node or NULL */
   struct rbt_node *left;
   /* Right child node or NULL */
   struct rbt_node *right;
   /* Parent node or NULL if root */
   struct rbt_node *parent;
   /* Node color */
   enum rbt_color color;
};

typedef void (*rbt_visitor_t)(struct rbt_node *n, void *data);
/* Returns 0 if `n == o`, < 0 if `n < o`, > 0 if `n > o` */
typedef int (*rbt_comp_t)(const struct rbt_node *n, const struct rbt_node *o);
/* Returns 0 if `key(n) == k`, -1 if `key(n) < k`, +1 if `key(n) > k` */
typedef int (*rbt_find_t)(const struct rbt_node *n, const void *k);

struct rbtree {
   struct rbt_node *root;
};

/* Initialize an empty tree*/
static void rbt_init(struct rbtree *t) {
   t->root = NULL;
}

/* Visit the entire tree (in order from lowest to highest value) and
   run `visitor` on each node */
void rbt_visit(struct rbtree *t, rbt_visitor_t, void *data);
/* Retreive the size (number of nodes) of the tree */
size_t rbt_size(struct rbtree *t);
struct rbt_node *_rbt_balance(struct rbt_node *n);

/* Search for a node matching `key` in the tree and return it. If no
   node is found returns NULL. */
static struct rbt_node *rbt_find(struct rbtree *t,
                                 rbt_find_t find_f,
                                 const void *key) {
   struct rbt_node *n = t->root;

   while (n) {
      int c = find_f(n, key);

      if (c == 0) {
         /* Found */
         return n;
      }

      if (c > 0) {
         n = n->left;
      } else {
         n = n->right;
      }
   }

   /* Not found */
   return NULL;
}

static struct rbt_node *_rbt_node_insert(struct rbt_node *p,
                                         struct rbt_node *n,
                                         rbt_comp_t comp_f) {

   int c = comp_f(p, n);

   if (c == 0) {
      struct rbt_node *gp = p->parent;

      /* We got a duplicate: replace and return the old value */
      *n = *p;
      if (gp) {
         if (gp->left == p) {
            gp->left = n;
         } else {
            gp->right = n;
         }
      }

      if (n->left) {
         n->left->parent = n;
      }

      if (n->right) {
         n->right->parent = n;
      }

      p->parent = NULL;
      p->left = NULL;
      p->right = NULL;

      return p;
   }

   if (c > 0) {
      if (p->left != NULL) {
         return _rbt_node_insert(p->left, n, comp_f);
      } else {
         n->parent = p;
         p->left = n;
         return NULL;
      }
   } else {
      if (p->right != NULL) {
         return _rbt_node_insert(p->right, n, comp_f);
      } else {
         n->parent = p;
         p->right = n;
         return NULL;
      }
   }
}

/* Insert `n` in `t`. If a node with the same key exists it's removed
   from the tree and returned, otherwise NULL is returned. */
static struct rbt_node *rbt_insert(struct rbtree *t,
                                   struct rbt_node *n,
                                   rbt_comp_t comp_f) {
   struct rbt_node *ret;

   n->left = NULL;
   n->right = NULL;

   if (t->root == NULL) {
      /* First node, it's the root */
      t->root = n;
      n->parent = NULL;
      n->left = NULL;
      n->right = NULL;
      n->color = RBT_BLACK;
      return NULL;
   }

   n->color = RBT_RED;
   ret = _rbt_node_insert(t->root, n, comp_f);

   if (ret) {
      if (ret == t->root) {
         t->root = n;
      }
      /* We replaced an existing node, no balancing necessary. */
      return ret;
   } else {
      /* We've inserted a new node, we may need to rebalance the
         tree */
      ret = _rbt_balance(n);
      if (ret) {
         /* Root changed */
         t->root = ret;
      }

      return NULL;
   }
}

#ifdef __cplusplus
}
#endif

#endif /* __RBTREE_H_ */
