#include <stdlib.h>
#include <assert.h>
#include "rbtree.h"

void rbt_init(struct rbtree *t) {
   t->root = NULL;
}

static struct rbt_node *rbt_node_insert(struct rbt_node *p,
                                        struct rbt_node *n) {

   if (p->key == n->key) {
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

   if (p->key > n->key) {
      if (p->left != NULL) {
         return rbt_node_insert(p->left, n);
      } else {
         n->parent = p;
         p->left = n;
         return NULL;
      }
   } else {
      if (p->right != NULL) {
         return rbt_node_insert(p->right, n);
      } else {
         n->parent = p;
         p->right = n;
         return NULL;
      }
   }
}

static struct rbt_node *rbt_sibling(struct rbt_node *n) {
   struct rbt_node *p = n->parent;

   if (p) {
      if (n == p->left) {
         return p->right;
      }
      return p->left;
   }

   return NULL;
}

static struct rbt_node *rbt_uncle(struct rbt_node *n) {
   if (n->parent) {
      return rbt_sibling(n->parent);
   }

   return NULL;
}

/* Rotate n with n->right:
 *
 *      N                 R
 *     / \               / \
 *    x   R      =>     N   z
 *       / \           / \
 *      y   z         x    y
 */
static void rbt_rotate_left(struct rbt_node *n) {
   struct rbt_node *r = n->right;
   struct rbt_node *y;
   struct rbt_node *p = n->parent;

   assert(r != NULL);

   y = r->left;

   n->right = y;
   r->left = n;

   r->parent = n->parent;
   n->parent = r;

   if (y) {
      y->parent = n;
   }

   if (p) {
      if (p->left == n) {
         p->left = r;
      } else {
         p->right = r;
      }
   }
}

/* Rotate n with n->left;
 *
 *       N              L
 *      / \            / \
 *     L   z    =>    x   N
 *    / \                / \
 *   x   y              y   z
 */
static void rbt_rotate_right(struct rbt_node *n) {
   struct rbt_node *l = n->left;
   struct rbt_node *y;
   struct rbt_node *p = n->parent;

   assert(l != NULL);

   y = l->right;

   n->left = y;
   l->right = n;

   l->parent = n->parent;
   n->parent = l;

   if (y) {
      y->parent = n;
   }

   if (p) {
      if (p->left == n) {
         p->left = l;
      } else {
         p->right = l;
      }
   }
}

/* Rebalance the tree. Returns the new root if it changed, otherwise NULL */
static struct rbt_node *rbt_balance(struct rbt_node *n) {
   struct rbt_node *p = n->parent;

   if (p == NULL) {
      /* We're the root */
      n->color = RBT_BLACK;
      return p;
   }

   if (p->color == RBT_BLACK) {
      /* We're already balanced, nothing to do */
      return 0;
   } else {
      struct rbt_node *gp = p->parent;
      struct rbt_node *u;

      /* `p` isn't black so it can be the root node, so `gp` can't be NULL */
      assert(gp != NULL);

      u = rbt_uncle(n);

      if (u && rbt_uncle(n)->color == RBT_RED) {
         /* Both parent and uncle are RED, paint them black */
         p->color = RBT_BLACK;
         u->color = RBT_BLACK;

         /* In order to maintain the number of black nodes toward each
            leaf invariant we need to paint the GP RED and rebalance from there. */
         gp->color = RBT_RED;
         return rbt_balance(gp);
      } else {
         /* Parent is red, uncle is black. */
         /* First if the node is in the inside of the subtree starting
            from the grandparent we rotate it with its parent to put it
            outside. */
         if (gp->left && n == gp->left->right) {
            rbt_rotate_left(p);
            p = n;
            n = n->left;
         } else if (gp->right && n == gp->right->left) {
            rbt_rotate_right(p);
            p = n;
            n = n->right;
         }

         /* At this point we know that `n` is needs to be balanced and
            that it's at the outside of the subtree starting from its
            grandparent. We can now rotate with the grandparent to
            balance the tree */
         if (n == p->left) {
            rbt_rotate_right(gp);
         } else {
            rbt_rotate_left(gp);
         }
         p->color = RBT_BLACK;
         gp->color = RBT_RED;

         if (p->parent == NULL) {
            /* We're the new root */
            return p;
         } else {
            return NULL;
         }
      }
   }
}

struct rbt_node *rbt_insert(struct rbtree *t, struct rbt_node *n) {
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
   ret = rbt_node_insert(t->root, n);

   if (ret) {
      if (ret == t->root) {
         t->root = n;
      }
      /* We replaced an existing node, no balancing necessary. */
      return ret;
   } else {
      /* We've inserted a new node, we may need to rebalance the
         tree */
      ret = rbt_balance(n);
      if (ret) {
         /* Root changed */
         t->root = ret;
      }

      return NULL;
   }
}

struct rbt_node *rbt_find(struct rbtree *t, uint32_t key) {
   struct rbt_node *n = t->root;

   while (n) {
      if (n->key == key) {
         /* Found */
         return n;
      }

      if (n->key > key) {
         n = n->left;
      } else {
         n = n->right;
      }
   }

   /* Not found */
   return NULL;
}

static void rbt_node_visit(struct rbt_node *n,
                           void (*visitor)(struct rbt_node *)) {
   if (n) {
      rbt_node_visit(n->left, visitor);
      visitor(n);
      rbt_node_visit(n->right, visitor);
   }
}

void rbt_visit(struct rbtree *t, void (*visitor)(struct rbt_node *)) {
   rbt_node_visit(t->root, visitor);
}
