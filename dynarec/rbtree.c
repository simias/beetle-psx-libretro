#include <stdlib.h>
#include <assert.h>
#include "rbtree.h"

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
struct rbt_node *_rbt_balance(struct rbt_node *n) {
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
         return _rbt_balance(gp);
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

static void rbt_node_visit(struct rbt_node *n,
                           rbt_visitor_t visitor,
                           void *data) {
   if (n) {
      rbt_node_visit(n->left, visitor, data);
      visitor(n, data);
      rbt_node_visit(n->right, visitor, data);
   }
}

void rbt_visit(struct rbtree *t, rbt_visitor_t visitor, void *data) {
   rbt_node_visit(t->root, visitor, data);
}

static void rbt_size_visitor(struct rbt_node *n, void *data) {
   size_t *size = data;

   (*size)++;
}

size_t rbt_size(struct rbtree *t) {
   size_t size = 0;

   rbt_visit(t, rbt_size_visitor, &size);

   return size;
}
