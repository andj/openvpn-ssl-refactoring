/*
 *  OpenVPN -- An application to securely tunnel IP networks
 *             over a single TCP/UDP port, with support for SSL/TLS-based
 *             session authentication and key exchange,
 *             packet encryption, packet authentication, and
 *             packet compression.
 *
 *  Copyright (C) 2002-2010 OpenVPN Technologies, Inc. <sales@openvpn.net>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2
 *  as published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program (see the file COPYING included with this
 *  distribution); if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/*
 * Support routines for adding/deleting network routes.
 */

#include "syshead.h"

#include "common.h"
#include "error.h"
#include "route.h"
#include "misc.h"
#include "socket.h"
#include "manage.h"
#include "win32.h"
#include "options.h"

#include "memdbg.h"

static void delete_route (const struct route *r, const struct tuntap *tt, unsigned int flags, const struct env_set *es);
static void get_bypass_addresses (struct route_bypass *rb, const unsigned int flags);

#ifdef ENABLE_DEBUG

static void
print_bypass_addresses (const struct route_bypass *rb)
{
  struct gc_arena gc = gc_new ();
  int i;
  for (i = 0; i < rb->n_bypass; ++i)
    {
      msg (D_ROUTE, "ROUTE: bypass_host_route[%d]=%s",
	   i,
	   print_in_addr_t (rb->bypass[i], 0, &gc));
    }
  gc_free (&gc);
}

#endif

struct route_option_list *
new_route_option_list (const int max_routes, struct gc_arena *a)
{
  struct route_option_list *ret;
  ALLOC_VAR_ARRAY_CLEAR_GC (ret, struct route_option_list, struct route_option, max_routes, a);
  ret->capacity = max_routes;
  return ret;
}

struct route_ipv6_option_list *
new_route_ipv6_option_list (const int max_routes, struct gc_arena *a)
{
  struct route_ipv6_option_list *ret;
  ALLOC_VAR_ARRAY_CLEAR_GC (ret, struct route_ipv6_option_list, struct route_ipv6_option, max_routes, a);
  ret->capacity = max_routes;
  return ret;
}

struct route_option_list *
clone_route_option_list (const struct route_option_list *src, struct gc_arena *a)
{
  const size_t rl_size = array_mult_safe (sizeof(struct route_option), src->capacity, sizeof(struct route_option_list));
  struct route_option_list *ret = gc_malloc (rl_size, false, a);
  memcpy (ret, src, rl_size);
  return ret;
}

void
copy_route_option_list (struct route_option_list *dest, const struct route_option_list *src)
{
  const size_t src_size = array_mult_safe (sizeof(struct route_option), src->capacity, sizeof(struct route_option_list));
  if (src->n > dest->capacity)
    msg (M_FATAL, PACKAGE_NAME " ROUTE: (copy) number of route options in src (%d) is greater than route list capacity in dest (%d)", src->n, dest->capacity);
  memcpy (dest, src, src_size);
}

struct route_list *
new_route_list (const int max_routes, struct gc_arena *a)
{
  struct route_list *ret;
  ALLOC_VAR_ARRAY_CLEAR_GC (ret, struct route_list, struct route, max_routes, a);
  ret->capacity = max_routes;
  return ret;
}

struct route_ipv6_list *
new_route_ipv6_list (const int max_routes, struct gc_arena *a)
{
  struct route_ipv6_list *ret;
  ALLOC_VAR_ARRAY_CLEAR_GC (ret, struct route_ipv6_list, struct route_ipv6, max_routes, a);
  ret->capacity = max_routes;
  return ret;
}

static const char *
route_string (const struct route *r, struct gc_arena *gc)
{
  struct buffer out = alloc_buf_gc (256, gc);
  buf_printf (&out, "ROUTE network %s netmask %s gateway %s",
	      print_in_addr_t (r->network, 0, gc),
	      print_in_addr_t (r->netmask, 0, gc),
	      print_in_addr_t (r->gateway, 0, gc)
	      );
  if (r->metric_defined)
    buf_printf (&out, " metric %d", r->metric);
  return BSTR (&out);
}

static bool
is_route_parm_defined (const char *parm)
{
  if (!parm)
    return false;
  if (!strcmp (parm, "default"))
    return false;
  return true;
}

static void
setenv_route_addr (struct env_set *es, const char *key, const in_addr_t addr, int i)
{
  struct gc_arena gc = gc_new ();
  struct buffer name = alloc_buf_gc (256, &gc);
  if (i >= 0)
    buf_printf (&name, "route_%s_%d", key, i);
  else
    buf_printf (&name, "route_%s", key);
  setenv_str (es, BSTR (&name), print_in_addr_t (addr, 0, &gc));
  gc_free (&gc);
}

static bool
get_special_addr (const struct route_special_addr *spec,
		  const char *string,
		  in_addr_t *out,
		  bool *status)
{
  if (status)
    *status = true;
  if (!strcmp (string, "vpn_gateway"))
    {
      if (spec)
	{
	  if (spec->remote_endpoint_defined)
	    *out = spec->remote_endpoint;
	  else
	    {
	      msg (M_INFO, PACKAGE_NAME " ROUTE: vpn_gateway undefined");
	      if (status)
		*status = false;
	    }
	}
      return true;
    }
  else if (!strcmp (string, "net_gateway"))
    {
      if (spec)
	{
	  if (spec->net_gateway_defined)
	    *out = spec->net_gateway;
	  else
	    {
	      msg (M_INFO, PACKAGE_NAME " ROUTE: net_gateway undefined -- unable to get default gateway from system");
	      if (status)
		*status = false;
	    }
	}
      return true;
    }
  else if (!strcmp (string, "remote_host"))
    {
      if (spec)
	{
	  if (spec->remote_host_defined)
	    *out = spec->remote_host;
	  else
	    {
	      msg (M_INFO, PACKAGE_NAME " ROUTE: remote_host undefined");
	      if (status)
		*status = false;
	    }
	}
      return true;
    }
  return false;
}

bool
is_special_addr (const char *addr_str)
{
  if (addr_str)
    return get_special_addr (NULL, addr_str, NULL, NULL);
  else
    return false;
}

static bool
init_route (struct route *r,
	    struct resolve_list *network_list,
	    const struct route_option *ro,
	    const struct route_special_addr *spec)
{
  const in_addr_t default_netmask = ~0;
  bool status;

  r->option = ro;
  r->defined = false;

  /* network */

  if (!is_route_parm_defined (ro->network))
    {
      goto fail;
    }
  
  if (!get_special_addr (spec, ro->network, &r->network, &status))
    {
      r->network = getaddr_multi (
				  GETADDR_RESOLVE
				  | GETADDR_HOST_ORDER
				  | GETADDR_WARN_ON_SIGNAL,
				  ro->network,
				  0,
				  &status,
				  NULL,
				  network_list);
    }

  if (!status)
    goto fail;

  /* netmask */

  if (is_route_parm_defined (ro->netmask))
    {
      r->netmask = getaddr (
			    GETADDR_HOST_ORDER
			    | GETADDR_WARN_ON_SIGNAL,
			    ro->netmask,
			    0,
			    &status,
			    NULL);
      if (!status)
	goto fail;
    }
  else
    r->netmask = default_netmask;

  /* gateway */

  if (is_route_parm_defined (ro->gateway))
    {
      if (!get_special_addr (spec, ro->gateway, &r->gateway, &status))
	{
	  r->gateway = getaddr (
				GETADDR_RESOLVE
				| GETADDR_HOST_ORDER
				| GETADDR_WARN_ON_SIGNAL,
				ro->gateway,
				0,
				&status,
				NULL);
	}
      if (!status)
	goto fail;
    }
  else
    {
      if (spec->remote_endpoint_defined)
	r->gateway = spec->remote_endpoint;
      else
	{
	  msg (M_WARN, PACKAGE_NAME " ROUTE: " PACKAGE_NAME " needs a gateway parameter for a --route option and no default was specified by either --route-gateway or --ifconfig options");
	  goto fail;
	}
    }

  /* metric */

  r->metric_defined = false;
  r->metric = 0;
  if (is_route_parm_defined (ro->metric))
    {
      r->metric = atoi (ro->metric);
      if (r->metric < 0)
	{
	  msg (M_WARN, PACKAGE_NAME " ROUTE: route metric for network %s (%s) must be >= 0",
	       ro->network,
	       ro->metric);
	  goto fail;
	}
      r->metric_defined = true;
    }
  else if (spec->default_metric_defined)
    {
      r->metric = spec->default_metric;
      r->metric_defined = true;
    }

  r->defined = true;

  return true;

 fail:
  msg (M_WARN, PACKAGE_NAME " ROUTE: failed to parse/resolve route for host/network: %s",
       ro->network);
  r->defined = false;
  return false;
}

static bool
init_route_ipv6 (struct route_ipv6 *r6,
	         const struct route_ipv6_option *r6o,
	         const struct route_ipv6_list *rl6 )
{
  r6->option = r6o;
  r6->defined = false;

  if ( !get_ipv6_addr( r6o->prefix, &r6->network, &r6->netbits, NULL, M_WARN ))
    goto fail;

  /* gateway */
  if (is_route_parm_defined (r6o->gateway))
    {
      if ( inet_pton( AF_INET6, r6o->gateway, &r6->gateway ) != 1 )
        {
	  msg( M_WARN, PACKAGE_NAME "ROUTE6: cannot parse gateway spec '%s'", r6o->gateway );
        }
    }
  else if (rl6->remote_endpoint_defined)
    {
      r6->gateway = rl6->remote_endpoint_ipv6;
    }
  else
    {
      msg (M_WARN, PACKAGE_NAME " ROUTE6: " PACKAGE_NAME " needs a gateway parameter for a --route-ipv6 option and no default was specified by either --route-ipv6-gateway or --ifconfig-ipv6 options");
      goto fail;
    }

  /* metric */

  r6->metric_defined = false;
  r6->metric = 0;
  if (is_route_parm_defined (r6o->metric))
    {
      r6->metric = atoi (r6o->metric);
      if (r6->metric < 0)
	{
	  msg (M_WARN, PACKAGE_NAME " ROUTE: route metric for network %s (%s) must be >= 0",
	       r6o->prefix,
	       r6o->metric);
	  goto fail;
	}
      r6->metric_defined = true;
    }
  else if (rl6->default_metric_defined)
    {
      r6->metric = rl6->default_metric;
      r6->metric_defined = true;
    }

  r6->defined = true;

  return true;

 fail:
  msg (M_WARN, PACKAGE_NAME " ROUTE: failed to parse/resolve route for host/network: %s",
       r6o->prefix);
  r6->defined = false;
  return false;
}

void
add_route_to_option_list (struct route_option_list *l,
			  const char *network,
			  const char *netmask,
			  const char *gateway,
			  const char *metric)
{
  struct route_option *ro;
  if (l->n >= l->capacity)
    msg (M_FATAL, PACKAGE_NAME " ROUTE: cannot add more than %d routes -- please increase the max-routes option in the client configuration file",
	 l->capacity);
  ro = &l->routes[l->n];
  ro->network = network;
  ro->netmask = netmask;
  ro->gateway = gateway;
  ro->metric = metric;
  ++l->n;
}

void
add_route_ipv6_to_option_list (struct route_ipv6_option_list *l,
			  const char *prefix,
			  const char *gateway,
			  const char *metric)
{
  struct route_ipv6_option *ro;
  if (l->n >= l->capacity)
    msg (M_FATAL, PACKAGE_NAME " ROUTE: cannot add more than %d IPv6 routes -- please increase the max-routes option in the client configuration file",
	 l->capacity);
  ro = &l->routes_ipv6[l->n];
  ro->prefix = prefix;
  ro->gateway = gateway;
  ro->metric = metric;
  ++l->n;
}

void
clear_route_list (struct route_list *rl)
{
  const int capacity = rl->capacity;
  const size_t rl_size = array_mult_safe (sizeof(struct route), capacity, sizeof(struct route_list));
  memset(rl, 0, rl_size);
  rl->capacity = capacity;
}

void
clear_route_ipv6_list (struct route_ipv6_list *rl6)
{
  const int capacity = rl6->capacity;
  const size_t rl6_size = array_mult_safe (sizeof(struct route_ipv6), capacity, sizeof(struct route_ipv6_list));
  memset(rl6, 0, rl6_size);
  rl6->capacity = capacity;
}

void
route_list_add_default_gateway (struct route_list *rl,
				struct env_set *es,
				const in_addr_t addr)
{
  rl->spec.remote_endpoint = addr;
  rl->spec.remote_endpoint_defined = true;
  setenv_route_addr (es, "vpn_gateway", rl->spec.remote_endpoint, -1);
}

bool
init_route_list (struct route_list *rl,
		 const struct route_option_list *opt,
		 const char *remote_endpoint,
		 int default_metric,
		 in_addr_t remote_host,
		 struct env_set *es)
{
  struct gc_arena gc = gc_new ();
  bool ret = true;

  clear_route_list (rl);

  rl->flags = opt->flags;

  if (remote_host)
    {
      rl->spec.remote_host = remote_host;
      rl->spec.remote_host_defined = true;
    }

  if (default_metric)
    {
      rl->spec.default_metric = default_metric;
      rl->spec.default_metric_defined = true;
    }

  rl->spec.net_gateway_defined = get_default_gateway (&rl->spec.net_gateway, NULL);
  if (rl->spec.net_gateway_defined)
    {
      setenv_route_addr (es, "net_gateway", rl->spec.net_gateway, -1);
      dmsg (D_ROUTE, "ROUTE default_gateway=%s", print_in_addr_t (rl->spec.net_gateway, 0, &gc));
    }
  else
    {
      dmsg (D_ROUTE, "ROUTE: default_gateway=UNDEF");
    }

  if (rl->flags & RG_ENABLE)
    {
      get_bypass_addresses (&rl->spec.bypass, rl->flags);
#ifdef ENABLE_DEBUG
      print_bypass_addresses (&rl->spec.bypass);
#endif
    }

  if (is_route_parm_defined (remote_endpoint))
    {
      rl->spec.remote_endpoint = getaddr (
				     GETADDR_RESOLVE
				     | GETADDR_HOST_ORDER
				     | GETADDR_WARN_ON_SIGNAL,
				     remote_endpoint,
				     0,
				     &rl->spec.remote_endpoint_defined,
				     NULL);

      if (rl->spec.remote_endpoint_defined)
	{
	  setenv_route_addr (es, "vpn_gateway", rl->spec.remote_endpoint, -1);
	}
      else
	{
	  msg (M_WARN, PACKAGE_NAME " ROUTE: failed to parse/resolve default gateway: %s",
	       remote_endpoint);
	  ret = false;
	}
    }
  else
    rl->spec.remote_endpoint_defined = false;

  /* parse the routes from opt to rl */
  {
    int i, j = 0;
    bool warned = false;
    for (i = 0; i < opt->n; ++i)
      {
	struct resolve_list netlist;
	struct route r;
	int k;

        CLEAR(netlist);		/* init_route() will not always init this */

	if (!init_route (&r,
			 &netlist,
			 &opt->routes[i],
			 &rl->spec))
	  ret = false;
	else
	  {
	    if (!netlist.len)
	      {
		netlist.data[0] = r.network;
		netlist.len = 1;
	      }
	    for (k = 0; k < netlist.len; ++k)
	      {
		if (j < rl->capacity)
		  {
		    r.network = netlist.data[k];
		    rl->routes[j++] = r;
		  }
		else
		  {
		    if (!warned)
		      {
			msg (M_WARN, PACKAGE_NAME " ROUTE: routes dropped because number of expanded routes is greater than route list capacity (%d)", rl->capacity);
			warned = true;
		      }
		  }
	      }
	  }
      }
    rl->n = j;
  }

  gc_free (&gc);
  return ret;
}

bool
init_route_ipv6_list (struct route_ipv6_list *rl6,
		 const struct route_ipv6_option_list *opt6,
		 const char *remote_endpoint,
		 int default_metric,
		 struct env_set *es)
{
  struct gc_arena gc = gc_new ();
  bool ret = true;

  clear_route_ipv6_list (rl6);

  rl6->flags = opt6->flags;

  if (default_metric)
    {
      rl6->default_metric = default_metric;
      rl6->default_metric_defined = true;
    }

  /* "default_gateway" is stuff for "redirect-gateway", which we don't
   * do for IPv6 yet -> TODO
   */
    {
      dmsg (D_ROUTE, "ROUTE6: default_gateway=UNDEF");
    }

  if ( is_route_parm_defined( remote_endpoint ))
    {
      if ( inet_pton( AF_INET6, remote_endpoint, 
			&rl6->remote_endpoint_ipv6) == 1 )
        {
	  rl6->remote_endpoint_defined = true;
        }
      else
	{
	  msg (M_WARN, PACKAGE_NAME " ROUTE: failed to parse/resolve default gateway: %s", remote_endpoint);
          ret = false;
	}
    }
  else
    rl6->remote_endpoint_defined = false;


  if (!(opt6->n >= 0 && opt6->n <= rl6->capacity))
    msg (M_FATAL, PACKAGE_NAME " ROUTE6: (init) number of route options (%d) is greater than route list capacity (%d)", opt6->n, rl6->capacity);

  /* parse the routes from opt to rl6 */
  {
    int i, j = 0;
    for (i = 0; i < opt6->n; ++i)
      {
	if (!init_route_ipv6 (&rl6->routes_ipv6[j],
			      &opt6->routes_ipv6[i],
			      rl6 ))
	  ret = false;
	else
	  ++j;
      }
    rl6->n = j;
  }

  gc_free (&gc);
  return ret;
}

static void
add_route3 (in_addr_t network,
	    in_addr_t netmask,
	    in_addr_t gateway,
	    const struct tuntap *tt,
	    unsigned int flags,
	    const struct env_set *es)
{
  struct route r;
  CLEAR (r);
  r.defined = true;
  r.network = network;
  r.netmask = netmask;
  r.gateway = gateway;
  add_route (&r, tt, flags, es);
}

static void
del_route3 (in_addr_t network,
	    in_addr_t netmask,
	    in_addr_t gateway,
	    const struct tuntap *tt,
	    unsigned int flags,
	    const struct env_set *es)
{
  struct route r;
  CLEAR (r);
  r.defined = true;
  r.network = network;
  r.netmask = netmask;
  r.gateway = gateway;
  delete_route (&r, tt, flags, es);
}

static void
add_bypass_routes (struct route_bypass *rb,
		   in_addr_t gateway,
		   const struct tuntap *tt,
		   unsigned int flags,
		   const struct env_set *es)
{
  int i;
  for (i = 0; i < rb->n_bypass; ++i)
    {
      if (rb->bypass[i] != gateway)
	add_route3 (rb->bypass[i],
		    ~0,
		    gateway,
		    tt,
		    flags,
		    es);
    }
}

static void
del_bypass_routes (struct route_bypass *rb,
		   in_addr_t gateway,
		   const struct tuntap *tt,
		   unsigned int flags,
		   const struct env_set *es)
{
  int i;
  for (i = 0; i < rb->n_bypass; ++i)
    {
      if (rb->bypass[i] != gateway)
	del_route3 (rb->bypass[i],
		    ~0,
		    gateway,
		    tt,
		    flags,
		    es);
    }
}

static void
redirect_default_route_to_vpn (struct route_list *rl, const struct tuntap *tt, unsigned int flags, const struct env_set *es)
{
  const char err[] = "NOTE: unable to redirect default gateway --";

  if (rl->flags & RG_ENABLE)
    {
      if (!rl->spec.remote_endpoint_defined)
	{
	  msg (M_WARN, "%s VPN gateway parameter (--route-gateway or --ifconfig) is missing", err);
	}
      else if (!rl->spec.net_gateway_defined)
	{
	  msg (M_WARN, "%s Cannot read current default gateway from system", err);
	}
      else if (!rl->spec.remote_host_defined)
	{
	  msg (M_WARN, "%s Cannot obtain current remote host address", err);
	}
      else
	{
	  bool local = BOOL_CAST(rl->flags & RG_LOCAL);
	  if (rl->flags & RG_AUTO_LOCAL) {
	    const int tla = test_local_addr (rl->spec.remote_host);
	    if (tla == TLA_NONLOCAL)
	      {
		dmsg (D_ROUTE, "ROUTE remote_host is NOT LOCAL");
		local = false;
	      }
	    else if (tla == TLA_LOCAL)
	      {
		dmsg (D_ROUTE, "ROUTE remote_host is LOCAL");
		local = true;
	      }
	  }
	  if (!local)
	    {
	      /* route remote host to original default gateway */
#ifdef USE_PF_INET6
	      /* if remote_host is not ipv4 (ie: ipv6), just skip
	       * adding this special /32 route */
	      if (rl->spec.remote_host != IPV4_INVALID_ADDR) {
#endif
		add_route3 (rl->spec.remote_host,
			    ~0,
			    rl->spec.net_gateway,
			    tt,
			    flags,
			    es);
		rl->did_local = true;
#ifdef USE_PF_INET6
	      } else {
		dmsg (D_ROUTE, "ROUTE remote_host protocol differs from tunneled");
	      }
#endif
	    }

	  /* route DHCP/DNS server traffic through original default gateway */
	  add_bypass_routes (&rl->spec.bypass, rl->spec.net_gateway, tt, flags, es);

	  if (rl->flags & RG_REROUTE_GW)
	    {
	      if (rl->flags & RG_DEF1)
		{
		  /* add new default route (1st component) */
		  add_route3 (0x00000000,
			      0x80000000,
			      rl->spec.remote_endpoint,
			      tt,
			      flags,
			      es);

		  /* add new default route (2nd component) */
		  add_route3 (0x80000000,
			      0x80000000,
			      rl->spec.remote_endpoint,
			      tt,
			      flags,
			      es);
		}
	      else
		{
		  /* delete default route */
		  del_route3 (0,
			      0,
			      rl->spec.net_gateway,
			      tt,
			      flags,
			      es);

		  /* add new default route */
		  add_route3 (0,
			      0,
			      rl->spec.remote_endpoint,
			      tt,
			      flags,
			      es);
		}
	    }

	  /* set a flag so we can undo later */
	  rl->did_redirect_default_gateway = true;
	}
    }
}

static void
undo_redirect_default_route_to_vpn (struct route_list *rl, const struct tuntap *tt, unsigned int flags, const struct env_set *es)
{
  if (rl->did_redirect_default_gateway)
    {
      /* delete remote host route */
      if (rl->did_local)
	{
	  del_route3 (rl->spec.remote_host,
		      ~0,
		      rl->spec.net_gateway,
		      tt,
		      flags,
		      es);
	  rl->did_local = false;
	}

      /* delete special DHCP/DNS bypass route */
      del_bypass_routes (&rl->spec.bypass, rl->spec.net_gateway, tt, flags, es);

      if (rl->flags & RG_REROUTE_GW)
	{
	  if (rl->flags & RG_DEF1)
	    {
	      /* delete default route (1st component) */
	      del_route3 (0x00000000,
			  0x80000000,
			  rl->spec.remote_endpoint,
			  tt,
			  flags,
			  es);

	      /* delete default route (2nd component) */
	      del_route3 (0x80000000,
			  0x80000000,
			  rl->spec.remote_endpoint,
			  tt,
			  flags,
			  es);
	    }
	  else
	    {
	      /* delete default route */
	      del_route3 (0,
			  0,
			  rl->spec.remote_endpoint,
			  tt,
			  flags,
			  es);

	      /* restore original default route */
	      add_route3 (0,
			  0,
			  rl->spec.net_gateway,
			  tt,
			  flags,
			  es);
	    }
	}

      rl->did_redirect_default_gateway = false;
    }
}

void
add_routes (struct route_list *rl, struct route_ipv6_list *rl6,
	    const struct tuntap *tt, unsigned int flags, const struct env_set *es)
{
  if (rl) 
      redirect_default_route_to_vpn (rl, tt, flags, es);

  if (rl && !rl->routes_added)
    {
      int i;

#ifdef ENABLE_MANAGEMENT
      if (management && rl->n)
	{
	  management_set_state (management,
				OPENVPN_STATE_ADD_ROUTES,
				NULL,
				0,
				0);
	}
#endif
      
      for (i = 0; i < rl->n; ++i)
	{
	  struct route *r = &rl->routes[i];
	  check_subnet_conflict (r->network, r->netmask, "route");
	  if (flags & ROUTE_DELETE_FIRST)
	    delete_route (r, tt, flags, es);
	  add_route (r, tt, flags, es);
	}
      rl->routes_added = true;
    }

  if (rl6 && !rl6->routes_added)
    {
      int i;

      for (i = 0; i < rl6->n; ++i)
	{
	  struct route_ipv6 *r = &rl6->routes_ipv6[i];
	  if (flags & ROUTE_DELETE_FIRST)
	    delete_route_ipv6 (r, tt, flags, es);
	  add_route_ipv6 (r, tt, flags, es);
	}
      rl6->routes_added = true;
    }
}

void
delete_routes (struct route_list *rl, struct route_ipv6_list *rl6,
	       const struct tuntap *tt, unsigned int flags, const struct env_set *es)
{
  if (rl && rl->routes_added)
    {
      int i;
      for (i = rl->n - 1; i >= 0; --i)
	{
	  const struct route *r = &rl->routes[i];
	  delete_route (r, tt, flags, es);
	}
      rl->routes_added = false;
    }

  if ( rl )
    {
      undo_redirect_default_route_to_vpn (rl, tt, flags, es);
      clear_route_list (rl);
    }

  if ( rl6 && rl6->routes_added )
    {
      int i;
      for (i = rl6->n - 1; i >= 0; --i)
	{
	  const struct route_ipv6 *r6 = &rl6->routes_ipv6[i];
	  delete_route_ipv6 (r6, tt, flags, es);
	}
      rl6->routes_added = false;
    }

  if ( rl6 )
    {
      clear_route_ipv6_list (rl6);
    }
}

#ifdef ENABLE_DEBUG

static const char *
show_opt (const char *option)
{
  if (!option)
    return "nil";
  else
    return option;
}

static void
print_route_option (const struct route_option *ro, int level)
{
  msg (level, "  route %s/%s/%s/%s",
       show_opt (ro->network),
       show_opt (ro->netmask),
       show_opt (ro->gateway),
       show_opt (ro->metric));
}

void
print_route_options (const struct route_option_list *rol,
		     int level)
{
  int i;
  if (rol->flags & RG_ENABLE)
    msg (level, "  [redirect_default_gateway local=%d]",
	 (rol->flags & RG_LOCAL) != 0);
  for (i = 0; i < rol->n; ++i)
    print_route_option (&rol->routes[i], level);
}

#endif

static void
print_route (const struct route *r, int level)
{
  struct gc_arena gc = gc_new ();
  if (r->defined)
    msg (level, "%s", route_string (r, &gc));
  gc_free (&gc);
}

void
print_routes (const struct route_list *rl, int level)
{
  int i;
  for (i = 0; i < rl->n; ++i)
    print_route (&rl->routes[i], level);
}

static void
setenv_route (struct env_set *es, const struct route *r, int i)
{
  struct gc_arena gc = gc_new ();
  if (r->defined)
    {
      setenv_route_addr (es, "network", r->network, i);
      setenv_route_addr (es, "netmask", r->netmask, i);
      setenv_route_addr (es, "gateway", r->gateway, i);

      if (r->metric_defined)
	{
	  struct buffer name = alloc_buf_gc (256, &gc);
	  buf_printf (&name, "route_metric_%d", i);
	  setenv_int (es, BSTR (&name), r->metric);
	}
    }
  gc_free (&gc);
}

void
setenv_routes (struct env_set *es, const struct route_list *rl)
{
  int i;
  for (i = 0; i < rl->n; ++i)
    setenv_route (es, &rl->routes[i], i + 1);
}

static void
setenv_route_ipv6 (struct env_set *es, const struct route_ipv6 *r6, int i)
{
  struct gc_arena gc = gc_new ();
  if (r6->defined)
    {
      struct buffer name1 = alloc_buf_gc( 256, &gc );
      struct buffer val = alloc_buf_gc( 256, &gc );
      struct buffer name2 = alloc_buf_gc( 256, &gc );

      buf_printf( &name1, "route_ipv6_network_%d", i );
      buf_printf( &val, "%s/%d", print_in6_addr( r6->network, 0, &gc ),
				 r6->netbits );
      setenv_str( es, BSTR(&name1), BSTR(&val) );

      buf_printf( &name2, "route_ipv6_gateway_%d", i );
      setenv_str( es, BSTR(&name2), print_in6_addr( r6->gateway, 0, &gc ));
    }
  gc_free (&gc);
}
void
setenv_routes_ipv6 (struct env_set *es, const struct route_ipv6_list *rl6)
{
  int i;
  for (i = 0; i < rl6->n; ++i)
    setenv_route_ipv6 (es, &rl6->routes_ipv6[i], i + 1);
}

void
add_route (struct route *r, const struct tuntap *tt, unsigned int flags, const struct env_set *es)
{
  struct gc_arena gc;
  struct argv argv;
  const char *network;
  const char *netmask;
  const char *gateway;
  bool status = false;

  if (!r->defined)
    return;

  gc_init (&gc);
  argv_init (&argv);

  network = print_in_addr_t (r->network, 0, &gc);
  netmask = print_in_addr_t (r->netmask, 0, &gc);
  gateway = print_in_addr_t (r->gateway, 0, &gc);

  /*
   * Filter out routes which are essentially no-ops
   */
  if (r->network == r->gateway && r->netmask == 0xFFFFFFFF)
    {
      msg (M_INFO, PACKAGE_NAME " ROUTE: omitted no-op route: %s/%s -> %s",
	   network, netmask, gateway);
      goto done;
    }

#if defined(TARGET_LINUX)
#ifdef CONFIG_FEATURE_IPROUTE
  argv_printf (&argv, "%s route add %s/%d via %s",
  	      iproute_path,
	      network,
	      count_netmask_bits(netmask),
	      gateway);
  if (r->metric_defined)
    argv_printf_cat (&argv, "metric %d", r->metric);

#else
  argv_printf (&argv, "%s add -net %s netmask %s gw %s",
		ROUTE_PATH,
	      network,
	      netmask,
	      gateway);
  if (r->metric_defined)
    argv_printf_cat (&argv, "metric %d", r->metric);
#endif  /*CONFIG_FEATURE_IPROUTE*/
  argv_msg (D_ROUTE, &argv);
  status = openvpn_execve_check (&argv, es, 0, "ERROR: Linux route add command failed");

#elif defined (WIN32)

  argv_printf (&argv, "%s%sc ADD %s MASK %s %s",
	       get_win_sys_path(),
	       WIN_ROUTE_PATH_SUFFIX,
	       network,
	       netmask,
	       gateway);
  if (r->metric_defined)
    argv_printf_cat (&argv, "METRIC %d", r->metric);

  argv_msg (D_ROUTE, &argv);

  if ((flags & ROUTE_METHOD_MASK) == ROUTE_METHOD_IPAPI)
    {
      status = add_route_ipapi (r, tt);
      msg (D_ROUTE, "Route addition via IPAPI %s", status ? "succeeded" : "failed");
    }
  else if ((flags & ROUTE_METHOD_MASK) == ROUTE_METHOD_EXE)
    {
      netcmd_semaphore_lock ();
      status = openvpn_execve_check (&argv, es, 0, "ERROR: Windows route add command failed");
      netcmd_semaphore_release ();
    }
  else if ((flags & ROUTE_METHOD_MASK) == ROUTE_METHOD_ADAPTIVE)
    {
      status = add_route_ipapi (r, tt);
      msg (D_ROUTE, "Route addition via IPAPI %s [adaptive]", status ? "succeeded" : "failed");
      if (!status)
	{
	  msg (D_ROUTE, "Route addition fallback to route.exe");
	  netcmd_semaphore_lock ();
	  status = openvpn_execve_check (&argv, es, 0, "ERROR: Windows route add command failed [adaptive]");
	  netcmd_semaphore_release ();
	}
    }
  else
    {
      ASSERT (0);
    }

#elif defined (TARGET_SOLARIS)

  /* example: route add 192.0.2.32 -netmask 255.255.255.224 somegateway */

  argv_printf (&argv, "%s add",
		ROUTE_PATH);

  argv_printf_cat (&argv, "%s -netmask %s %s",
	      network,
	      netmask,
	      gateway);

  if (r->metric_defined)
    argv_printf_cat (&argv, "%d", r->metric);

  argv_msg (D_ROUTE, &argv);
  status = openvpn_execve_check (&argv, es, 0, "ERROR: Solaris route add command failed");

#elif defined(TARGET_FREEBSD)

  argv_printf (&argv, "%s add",
		ROUTE_PATH);

#if 0
  if (r->metric_defined)
    argv_printf_cat (&argv, "-rtt %d", r->metric);
#endif

  argv_printf_cat (&argv, "-net %s %s %s",
	      network,
	      gateway,
	      netmask);

  argv_msg (D_ROUTE, &argv);
  status = openvpn_execve_check (&argv, es, 0, "ERROR: FreeBSD route add command failed");

#elif defined(TARGET_DRAGONFLY)

  argv_printf (&argv, "%s add",
		ROUTE_PATH);

#if 0
  if (r->metric_defined)
    argv_printf_cat (&argv, "-rtt %d", r->metric);
#endif

  argv_printf_cat (&argv, "-net %s %s %s",
	      network,
	      gateway,
	      netmask);

  argv_msg (D_ROUTE, &argv);
  status = openvpn_execve_check (&argv, es, 0, "ERROR: DragonFly route add command failed");

#elif defined(TARGET_DARWIN)

  argv_printf (&argv, "%s add",
		ROUTE_PATH);

#if 0
  if (r->metric_defined)
    argv_printf_cat (&argv, "-rtt %d", r->metric);
#endif

  argv_printf_cat (&argv, "-net %s %s %s",
              network,
              gateway,
              netmask);

  argv_msg (D_ROUTE, &argv);
  status = openvpn_execve_check (&argv, es, 0, "ERROR: OS X route add command failed");

#elif defined(TARGET_OPENBSD) || defined(TARGET_NETBSD)

  argv_printf (&argv, "%s add",
		ROUTE_PATH);

#if 0
  if (r->metric_defined)
    argv_printf_cat (&argv, "-rtt %d", r->metric);
#endif

  argv_printf_cat (&argv, "-net %s %s -netmask %s",
	      network,
	      gateway,
	      netmask);

  argv_msg (D_ROUTE, &argv);
  status = openvpn_execve_check (&argv, es, 0, "ERROR: OpenBSD/NetBSD route add command failed");

#else
  msg (M_FATAL, "Sorry, but I don't know how to do 'route' commands on this operating system.  Try putting your routes in a --route-up script");
#endif

 done:
  r->defined = status;
  argv_reset (&argv);
  gc_free (&gc);
}


static const char * 
print_in6_addr_netbits_only( struct in6_addr network_copy, int netbits, 
                             struct gc_arena * gc)
{
  /* clear host bit parts of route 
   * (needed if routes are specified improperly, or if we need to 
   * explicitely setup/clear the "connected" network routes on some OSes)
   */
  int byte = 15;
  int bits_to_clear = 128 - netbits;

  while( byte >= 0 && bits_to_clear > 0 )
    {
      if ( bits_to_clear >= 8 )
	{ network_copy.s6_addr[byte--] = 0; bits_to_clear -= 8; }
      else
	{ network_copy.s6_addr[byte--] &= (~0 << bits_to_clear); bits_to_clear = 0; }
    }

  return print_in6_addr( network_copy, 0, gc);
}

void
add_route_ipv6 (struct route_ipv6 *r6, const struct tuntap *tt, unsigned int flags, const struct env_set *es)
{
  struct gc_arena gc;
  struct argv argv;

  const char *network;
  const char *gateway;
  bool status = false;
  const char *device = tt->actual_name;

  if (!r6->defined)
    return;

  gc_init (&gc);
  argv_init (&argv);

  network = print_in6_addr_netbits_only( r6->network, r6->netbits, &gc);
  gateway = print_in6_addr( r6->gateway, 0, &gc);

  if ( !tt->ipv6 )
    {
      msg( M_INFO, "add_route_ipv6(): not adding %s/%d, no IPv6 on if %s",
		    network, r6->netbits, device );
      return;
    }

  msg( M_INFO, "add_route_ipv6(%s/%d -> %s metric %d) dev %s",
		network, r6->netbits, gateway, r6->metric, device );

  /*
   * Filter out routes which are essentially no-ops
   * (not currently done for IPv6)
   */

#if defined(TARGET_LINUX)
#ifdef CONFIG_FEATURE_IPROUTE
  argv_printf (&argv, "%s -6 route add %s/%d dev %s",
  	      iproute_path,
	      network,
	      r6->netbits,
	      device);
  if (r6->metric_defined)
    argv_printf_cat (&argv, " metric %d", r6->metric);

#else
  argv_printf (&argv, "%s -A inet6 add %s/%d dev %s",
		ROUTE_PATH,
	      network,
	      r6->netbits,
	      device);
  if (r6->metric_defined)
    argv_printf_cat (&argv, " metric %d", r6->metric);
#endif  /*CONFIG_FEATURE_IPROUTE*/
  argv_msg (D_ROUTE, &argv);
  status = openvpn_execve_check (&argv, es, 0, "ERROR: Linux route -6/-A inet6 add command failed");

#elif defined (WIN32)

  /* netsh interface ipv6 add route 2001:db8::/32 MyTunDevice */
  argv_printf (&argv, "%s%sc interface ipv6 add route %s/%d %s",
	       get_win_sys_path(),
	       NETSH_PATH_SUFFIX,
	       network,
	       r6->netbits,
	       device);

  /* next-hop depends on TUN or TAP mode:
   * - in TAP mode, we use the "real" next-hop
   * - in TUN mode we use a special-case link-local address that the tapdrvr
   *   knows about and will answer ND (neighbor discovery) packets for
   */
  if ( tt->type == DEV_TYPE_TUN )
	argv_printf_cat( &argv, " %s", "fe80::8" );
  else
	argv_printf_cat( &argv, " %s", gateway );

#if 0
  if (r->metric_defined)
    argv_printf_cat (&argv, " METRIC %d", r->metric);
#endif

  /* in some versions of Windows, routes are persistent across reboots by
   * default, unless "store=active" is set (pointed out by Tony Lim, thanks)
   */
  argv_printf_cat( &argv, " store=active" );

  argv_msg (D_ROUTE, &argv);

  netcmd_semaphore_lock ();
  status = openvpn_execve_check (&argv, es, 0, "ERROR: Windows route add ipv6 command failed");
  netcmd_semaphore_release ();

#elif defined (TARGET_SOLARIS)

  /* example: route add -inet6 2001:db8::/32 somegateway 0 */

  /* for some weird reason, this does not work for me unless I set
   * "metric 0" - otherwise, the routes will be nicely installed, but
   * packets will just disappear somewhere.  So we use "0" now...
   */

  argv_printf (&argv, "%s add -inet6 %s/%d %s 0",
		ROUTE_PATH,
		network,
		r6->netbits,
		gateway );

  argv_msg (D_ROUTE, &argv);
  status = openvpn_execve_check (&argv, es, 0, "ERROR: Solaris route add -inet6 command failed");

#elif defined(TARGET_FREEBSD) || defined(TARGET_DRAGONFLY)

  argv_printf (&argv, "%s add -inet6 %s/%d -iface %s",
		ROUTE_PATH,
	        network,
	        r6->netbits,
	        device );

  argv_msg (D_ROUTE, &argv);
  status = openvpn_execve_check (&argv, es, 0, "ERROR: *BSD route add -inet6 command failed");

#elif defined(TARGET_DARWIN) 

  argv_printf (&argv, "%s add -inet6 %s -prefixlen %d -iface %s",
		ROUTE_PATH,
	        network, r6->netbits, device );

  argv_msg (D_ROUTE, &argv);
  status = openvpn_execve_check (&argv, es, 0, "ERROR: MacOS X route add -inet6 command failed");

#elif defined(TARGET_OPENBSD)

  argv_printf (&argv, "%s add -inet6 %s -prefixlen %d %s",
		ROUTE_PATH,
	        network, r6->netbits, gateway );

  argv_msg (D_ROUTE, &argv);
  status = openvpn_execve_check (&argv, es, 0, "ERROR: OpenBSD route add -inet6 command failed");

#elif defined(TARGET_NETBSD)

  argv_printf (&argv, "%s add -inet6 %s/%d %s",
		ROUTE_PATH,
	        network, r6->netbits, gateway );

  argv_msg (D_ROUTE, &argv);
  status = openvpn_execve_check (&argv, es, 0, "ERROR: NetBSD route add -inet6 command failed");

#else
  msg (M_FATAL, "Sorry, but I don't know how to do 'route ipv6' commands on this operating system.  Try putting your routes in a --route-up script");
#endif

  r6->defined = status;
  argv_reset (&argv);
  gc_free (&gc);
}

static void
delete_route (const struct route *r, const struct tuntap *tt, unsigned int flags, const struct env_set *es)
{
  struct gc_arena gc;
  struct argv argv;
  const char *network;
  const char *netmask;
  const char *gateway;

  if (!r->defined)
    return;

  gc_init (&gc);
  argv_init (&argv);

  network = print_in_addr_t (r->network, 0, &gc);
  netmask = print_in_addr_t (r->netmask, 0, &gc);
  gateway = print_in_addr_t (r->gateway, 0, &gc);

#if defined(TARGET_LINUX)
#ifdef CONFIG_FEATURE_IPROUTE
  argv_printf (&argv, "%s route del %s/%d",
  	      iproute_path,
	      network,
	      count_netmask_bits(netmask));
#else

  argv_printf (&argv, "%s del -net %s netmask %s",
		ROUTE_PATH,
	      network,
	      netmask);
#endif /*CONFIG_FEATURE_IPROUTE*/
  if (r->metric_defined)
    argv_printf_cat (&argv, "metric %d", r->metric);
  argv_msg (D_ROUTE, &argv);
  openvpn_execve_check (&argv, es, 0, "ERROR: Linux route delete command failed");

#elif defined (WIN32)
  
  argv_printf (&argv, "%s%sc DELETE %s MASK %s %s",
	       get_win_sys_path(),
	       WIN_ROUTE_PATH_SUFFIX,
	       network,
	       netmask,
	       gateway);

  argv_msg (D_ROUTE, &argv);

  if ((flags & ROUTE_METHOD_MASK) == ROUTE_METHOD_IPAPI)
    {
      const bool status = del_route_ipapi (r, tt);
      msg (D_ROUTE, "Route deletion via IPAPI %s", status ? "succeeded" : "failed");
    }
  else if ((flags & ROUTE_METHOD_MASK) == ROUTE_METHOD_EXE)
    {
      netcmd_semaphore_lock ();
      openvpn_execve_check (&argv, es, 0, "ERROR: Windows route delete command failed");
      netcmd_semaphore_release ();
    }
  else if ((flags & ROUTE_METHOD_MASK) == ROUTE_METHOD_ADAPTIVE)
    {
      const bool status = del_route_ipapi (r, tt);
      msg (D_ROUTE, "Route deletion via IPAPI %s [adaptive]", status ? "succeeded" : "failed");
      if (!status)
	{
	  msg (D_ROUTE, "Route deletion fallback to route.exe");
	  netcmd_semaphore_lock ();
	  openvpn_execve_check (&argv, es, 0, "ERROR: Windows route delete command failed [adaptive]");
	  netcmd_semaphore_release ();
	}
    }
  else
    {
      ASSERT (0);
    }

#elif defined (TARGET_SOLARIS)

  argv_printf (&argv, "%s delete %s -netmask %s %s",
		ROUTE_PATH,
	      network,
	      netmask,
	      gateway);

  argv_msg (D_ROUTE, &argv);
  openvpn_execve_check (&argv, es, 0, "ERROR: Solaris route delete command failed");

#elif defined(TARGET_FREEBSD)

  argv_printf (&argv, "%s delete -net %s %s %s",
		ROUTE_PATH,
	      network,
	      gateway,
	      netmask);

  argv_msg (D_ROUTE, &argv);
  openvpn_execve_check (&argv, es, 0, "ERROR: FreeBSD route delete command failed");

#elif defined(TARGET_DRAGONFLY)

  argv_printf (&argv, "%s delete -net %s %s %s",
		ROUTE_PATH,
	      network,
	      gateway,
	      netmask);

  argv_msg (D_ROUTE, &argv);
  openvpn_execve_check (&argv, es, 0, "ERROR: DragonFly route delete command failed");

#elif defined(TARGET_DARWIN)

  argv_printf (&argv, "%s delete -net %s %s %s",
		ROUTE_PATH,
              network,
              gateway,
              netmask);

  argv_msg (D_ROUTE, &argv);
  openvpn_execve_check (&argv, es, 0, "ERROR: OS X route delete command failed");

#elif defined(TARGET_OPENBSD) || defined(TARGET_NETBSD)

  argv_printf (&argv, "%s delete -net %s %s -netmask %s",
		ROUTE_PATH,
	      network,
	      gateway,
	      netmask);

  argv_msg (D_ROUTE, &argv);
  openvpn_execve_check (&argv, es, 0, "ERROR: OpenBSD/NetBSD route delete command failed");

#else
  msg (M_FATAL, "Sorry, but I don't know how to do 'route' commands on this operating system.  Try putting your routes in a --route-up script");
#endif

  argv_reset (&argv);
  gc_free (&gc);
}

void
delete_route_ipv6 (const struct route_ipv6 *r6, const struct tuntap *tt, unsigned int flags, const struct env_set *es)
{
  struct gc_arena gc;
  struct argv argv;
  const char *network;
  const char *gateway;
  const char *device = tt->actual_name;

  if (!r6->defined)
    return;

  gc_init (&gc);
  argv_init (&argv);

  network = print_in6_addr_netbits_only( r6->network, r6->netbits, &gc);
  gateway = print_in6_addr( r6->gateway, 0, &gc);

  if ( !tt->ipv6 )
    {
      msg( M_INFO, "delete_route_ipv6(): not deleting %s/%d, no IPv6 on if %s",
		    network, r6->netbits, device );
      return;
    }

  msg( M_INFO, "delete_route_ipv6(%s/%d)", network, r6->netbits );

#if defined(TARGET_LINUX)
#ifdef CONFIG_FEATURE_IPROUTE
  argv_printf (&argv, "%s -6 route del %s/%d dev %s",
  	      iproute_path,
	      network,
	      r6->netbits,
	      device);
#else
  argv_printf (&argv, "%s -A inet6 del %s/%d dev %s",
		ROUTE_PATH,
	      network,
	      r6->netbits,
	      device);
#endif  /*CONFIG_FEATURE_IPROUTE*/
  argv_msg (D_ROUTE, &argv);
  openvpn_execve_check (&argv, es, 0, "ERROR: Linux route -6/-A inet6 del command failed");

#elif defined (WIN32)

  /* netsh interface ipv6 delete route 2001:db8::/32 MyTunDevice */
  argv_printf (&argv, "%s%sc interface ipv6 delete route %s/%d %s",
	       get_win_sys_path(),
	       NETSH_PATH_SUFFIX,
	       network,
	       r6->netbits,
	       device);

  /* next-hop depends on TUN or TAP mode:
   * - in TAP mode, we use the "real" next-hop
   * - in TUN mode we use a special-case link-local address that the tapdrvr
   *   knows about and will answer ND (neighbor discovery) packets for
   * (and "route deletion without specifying next-hop" does not work...)
   */
  if ( tt->type == DEV_TYPE_TUN )
	argv_printf_cat( &argv, " %s", "fe80::8" );
  else
	argv_printf_cat( &argv, " %s", gateway );

#if 0
  if (r->metric_defined)
    argv_printf_cat (&argv, "METRIC %d", r->metric);
#endif

  argv_msg (D_ROUTE, &argv);

  netcmd_semaphore_lock ();
  openvpn_execve_check (&argv, es, 0, "ERROR: Windows route add ipv6 command failed");
  netcmd_semaphore_release ();

#elif defined (TARGET_SOLARIS)

  /* example: route delete -inet6 2001:db8::/32 somegateway */
  /* GERT-TODO: this is untested, but should work */

  argv_printf (&argv, "%s delete -inet6 %s/%d %s",
		ROUTE_PATH,
		network,
		r6->netbits,
		gateway );

  argv_msg (D_ROUTE, &argv);
  openvpn_execve_check (&argv, es, 0, "ERROR: Solaris route delete -inet6 command failed");

#elif defined(TARGET_FREEBSD) || defined(TARGET_DRAGONFLY)

  argv_printf (&argv, "%s delete -inet6 %s/%d -iface %s",
		ROUTE_PATH,
	        network,
	        r6->netbits,
	        device );

  argv_msg (D_ROUTE, &argv);
  openvpn_execve_check (&argv, es, 0, "ERROR: *BSD route delete -inet6 command failed");

#elif defined(TARGET_DARWIN) 

  argv_printf (&argv, "%s delete -inet6 %s -prefixlen %d -iface %s",
		ROUTE_PATH, 
		network, r6->netbits, device );

  argv_msg (D_ROUTE, &argv);
  openvpn_execve_check (&argv, es, 0, "ERROR: *BSD route delete -inet6 command failed");

#elif defined(TARGET_OPENBSD)

  argv_printf (&argv, "%s delete -inet6 %s -prefixlen %d %s",
		ROUTE_PATH,
	        network, r6->netbits, gateway );

  argv_msg (D_ROUTE, &argv);
  openvpn_execve_check (&argv, es, 0, "ERROR: OpenBSD route delete -inet6 command failed");

#elif defined(TARGET_NETBSD)

  argv_printf (&argv, "%s delete -inet6 %s/%d %s",
		ROUTE_PATH,
	        network, r6->netbits, gateway );

  argv_msg (D_ROUTE, &argv);
  openvpn_execve_check (&argv, es, 0, "ERROR: NetBSD route delete -inet6 command failed");

#else
  msg (M_FATAL, "Sorry, but I don't know how to do 'route ipv6' commands on this operating system.  Try putting your routes in a --route-down script");
#endif

  argv_reset (&argv);
  gc_free (&gc);
}

/*
 * The --redirect-gateway option requires OS-specific code below
 * to get the current default gateway.
 */

#if defined(WIN32)

static const MIB_IPFORWARDTABLE *
get_windows_routing_table (struct gc_arena *gc)
{
  ULONG size = 0;
  PMIB_IPFORWARDTABLE rt = NULL;
  DWORD status;

  status = GetIpForwardTable (NULL, &size, TRUE);
  if (status == ERROR_INSUFFICIENT_BUFFER)
    {
      rt = (PMIB_IPFORWARDTABLE) gc_malloc (size, false, gc);
      status = GetIpForwardTable (rt, &size, TRUE);
      if (status != NO_ERROR)
	{
	  msg (D_ROUTE, "NOTE: GetIpForwardTable returned error: %s (code=%u)",
	       strerror_win32 (status, gc),
	       (unsigned int)status);
	  rt = NULL;
	}
    }
  return rt;
}

static int
test_route (const IP_ADAPTER_INFO *adapters,
	    const in_addr_t gateway,
	    DWORD *index)
{
  int count = 0;
  DWORD i = adapter_index_of_ip (adapters, gateway, &count, NULL);
  if (index)
    *index = i;
  return count;
}

static void
test_route_helper (bool *ret,
		   int *count,
		   int *good,
		   int *ambig,
		   const IP_ADAPTER_INFO *adapters,
		   const in_addr_t gateway)
{
  int c;

  ++*count;
  c = test_route (adapters, gateway, NULL);
  if (c == 0)
    *ret = false;
  else
    ++*good;
  if (c > 1)
    ++*ambig;
}

/*
 * If we tried to add routes now, would we succeed?
 */
bool
test_routes (const struct route_list *rl, const struct tuntap *tt)
{
  struct gc_arena gc = gc_new ();
  const IP_ADAPTER_INFO *adapters = get_adapter_info_list (&gc);
  bool ret = false;
  int count = 0;
  int good = 0;
  int ambig = 0;
  bool adapter_up = false;

  if (is_adapter_up (tt, adapters))
    {
      ret = true;
      adapter_up = true;

      if (rl)
	{
	  int i;
	  for (i = 0; i < rl->n; ++i)
	    test_route_helper (&ret, &count, &good, &ambig, adapters, rl->routes[i].gateway);

	  if ((rl->flags & RG_ENABLE) && rl->spec.remote_endpoint_defined)
	    test_route_helper (&ret, &count, &good, &ambig, adapters, rl->spec.remote_endpoint);
	}
    }

  msg (D_ROUTE, "TEST ROUTES: %d/%d succeeded len=%d ret=%d a=%d u/d=%s",
       good,
       count,
       rl ? rl->n : -1,
       (int)ret,
       ambig,
       adapter_up ? "up" : "down");

  gc_free (&gc);
  return ret;
}

static const MIB_IPFORWARDROW *
get_default_gateway_row (const MIB_IPFORWARDTABLE *routes)
{
  struct gc_arena gc = gc_new ();
  DWORD lowest_metric = ~0;
  const MIB_IPFORWARDROW *ret = NULL;
  int i;
  int best = -1;

  if (routes)
    {
      for (i = 0; i < routes->dwNumEntries; ++i)
	{
	  const MIB_IPFORWARDROW *row = &routes->table[i];
	  const in_addr_t net = ntohl (row->dwForwardDest);
	  const in_addr_t mask = ntohl (row->dwForwardMask);
	  const DWORD index = row->dwForwardIfIndex;
	  const DWORD metric = row->dwForwardMetric1;

	  dmsg (D_ROUTE_DEBUG, "GDGR: route[%d] %s/%s i=%d m=%d",
		i,
		print_in_addr_t ((in_addr_t) net, 0, &gc),
		print_in_addr_t ((in_addr_t) mask, 0, &gc),
		(int)index,
		(int)metric);

	  if (!net && !mask && metric < lowest_metric)
	    {
	      ret = row;
	      lowest_metric = metric;
	      best = i;
	    }
	}
    }

  dmsg (D_ROUTE_DEBUG, "GDGR: best=%d lm=%u", best, (unsigned int)lowest_metric);

  gc_free (&gc);
  return ret;
}

bool
get_default_gateway (in_addr_t *gw, in_addr_t *netmask)
{
  struct gc_arena gc = gc_new ();
  bool ret_bool = false;

  const IP_ADAPTER_INFO *adapters = get_adapter_info_list (&gc);
  const MIB_IPFORWARDTABLE *routes = get_windows_routing_table (&gc);
  const MIB_IPFORWARDROW *row = get_default_gateway_row (routes);

  if (row)
    {
      *gw = ntohl (row->dwForwardNextHop);
      if (netmask)
	{
	  if (adapter_index_of_ip (adapters, *gw, NULL, netmask) == ~0)
	    *netmask = ~0;
	}
      ret_bool = true;
    }

  gc_free (&gc);
  return ret_bool;
}

static DWORD
windows_route_find_if_index (const struct route *r, const struct tuntap *tt)
{
  struct gc_arena gc = gc_new ();
  DWORD ret = ~0;
  int count = 0;
  const IP_ADAPTER_INFO *adapters = get_adapter_info_list (&gc);
  const IP_ADAPTER_INFO *tun_adapter = get_tun_adapter (tt, adapters);
  bool on_tun = false;

  /* first test on tun interface */
  if (is_ip_in_adapter_subnet (tun_adapter, r->gateway, NULL))
    {
      ret = tun_adapter->Index;
      count = 1;
      on_tun = true;
    }
  else /* test on other interfaces */
    {
      count = test_route (adapters, r->gateway, &ret);
    }

  if (count == 0)
    {
      msg (M_WARN, "Warning: route gateway is not reachable on any active network adapters: %s",
	   print_in_addr_t (r->gateway, 0, &gc));
      ret = ~0;
    }
  else if (count > 1)
    {
      msg (M_WARN, "Warning: route gateway is ambiguous: %s (%d matches)",
	   print_in_addr_t (r->gateway, 0, &gc),
	   count);
      ret = ~0;
    }

  dmsg (D_ROUTE_DEBUG, "DEBUG: route find if: on_tun=%d count=%d index=%d",
       on_tun,
       count,
       (int)ret);

  gc_free (&gc);
  return ret;
}

bool
add_route_ipapi (const struct route *r, const struct tuntap *tt)
{
  struct gc_arena gc = gc_new ();
  bool ret = false;
  DWORD status;
  const DWORD if_index = windows_route_find_if_index (r, tt);  

  if (if_index != ~0)
    {
      MIB_IPFORWARDROW fr;
      CLEAR (fr);
      fr.dwForwardDest = htonl (r->network);
      fr.dwForwardMask = htonl (r->netmask);
      fr.dwForwardPolicy = 0;
      fr.dwForwardNextHop = htonl (r->gateway);
      fr.dwForwardIfIndex = if_index;
      fr.dwForwardType = 4;  /* the next hop is not the final dest */
      fr.dwForwardProto = 3; /* PROTO_IP_NETMGMT */
      fr.dwForwardAge = 0;
      fr.dwForwardNextHopAS = 0;
      fr.dwForwardMetric1 = r->metric_defined ? r->metric : 1;
      fr.dwForwardMetric2 = ~0;
      fr.dwForwardMetric3 = ~0;
      fr.dwForwardMetric4 = ~0;
      fr.dwForwardMetric5 = ~0;

      if ((r->network & r->netmask) != r->network)
	msg (M_WARN, "Warning: address %s is not a network address in relation to netmask %s",
	     print_in_addr_t (r->network, 0, &gc),
	     print_in_addr_t (r->netmask, 0, &gc));

      status = CreateIpForwardEntry (&fr);

      if (status == NO_ERROR)
	ret = true;
      else
	{
	  /* failed, try increasing the metric to work around Vista issue */
	  const unsigned int forward_metric_limit = 2048; /* iteratively retry higher metrics up to this limit */

	  for ( ; fr.dwForwardMetric1 <= forward_metric_limit; ++fr.dwForwardMetric1)
	    {
	      /* try a different forward type=3 ("the next hop is the final dest") in addition to 4.
		 --redirect-gateway over RRAS seems to need this. */
	      for (fr.dwForwardType = 4; fr.dwForwardType >= 3; --fr.dwForwardType)
		{
		  status = CreateIpForwardEntry (&fr);
		  if (status == NO_ERROR)
		    {
		      msg (D_ROUTE, "ROUTE: CreateIpForwardEntry succeeded with dwForwardMetric1=%u and dwForwardType=%u",
			   (unsigned int)fr.dwForwardMetric1,
			   (unsigned int)fr.dwForwardType);
		      ret = true;
		      goto doublebreak;
		    }
		  else if (status != ERROR_BAD_ARGUMENTS)
		    goto doublebreak;
		}
	    }

	doublebreak:
	  if (status != NO_ERROR)
	    msg (M_WARN, "ROUTE: route addition failed using CreateIpForwardEntry: %s [status=%u if_index=%u]",
		 strerror_win32 (status, &gc),
		 (unsigned int)status,
		 (unsigned int)if_index);
	}
    }

  gc_free (&gc);
  return ret;
}

bool
del_route_ipapi (const struct route *r, const struct tuntap *tt)
{
  struct gc_arena gc = gc_new ();
  bool ret = false;
  DWORD status;
  const DWORD if_index = windows_route_find_if_index (r, tt);

  if (if_index != ~0)
    {
      MIB_IPFORWARDROW fr;
      CLEAR (fr);

      fr.dwForwardDest = htonl (r->network);
      fr.dwForwardMask = htonl (r->netmask);
      fr.dwForwardPolicy = 0;
      fr.dwForwardNextHop = htonl (r->gateway);
      fr.dwForwardIfIndex = if_index;

      status = DeleteIpForwardEntry (&fr);

      if (status == NO_ERROR)
	ret = true;
      else
	msg (M_WARN, "ROUTE: route deletion failed using DeleteIpForwardEntry: %s",
	     strerror_win32 (status, &gc));
    }

  gc_free (&gc);
  return ret;
}

static const char *
format_route_entry (const MIB_IPFORWARDROW *r, struct gc_arena *gc)
{
  struct buffer out = alloc_buf_gc (256, gc);
  buf_printf (&out, "%s %s %s p=%d i=%d t=%d pr=%d a=%d h=%d m=%d/%d/%d/%d/%d", 
	      print_in_addr_t (r->dwForwardDest, IA_NET_ORDER, gc),
	      print_in_addr_t (r->dwForwardMask, IA_NET_ORDER, gc),
	      print_in_addr_t (r->dwForwardNextHop, IA_NET_ORDER, gc),
	      (int)r->dwForwardPolicy,
	      (int)r->dwForwardIfIndex,
	      (int)r->dwForwardType,
	      (int)r->dwForwardProto,
	      (int)r->dwForwardAge,
	      (int)r->dwForwardNextHopAS,
	      (int)r->dwForwardMetric1,
	      (int)r->dwForwardMetric2,
	      (int)r->dwForwardMetric3,
	      (int)r->dwForwardMetric4,
	      (int)r->dwForwardMetric5);
  return BSTR (&out);
}

/*
 * Show current routing table
 */
void
show_routes (int msglev)
{
  struct gc_arena gc = gc_new ();
  int i;

  const MIB_IPFORWARDTABLE *rt = get_windows_routing_table (&gc);

  msg (msglev, "SYSTEM ROUTING TABLE");
  if (rt)
    {
      for (i = 0; i < rt->dwNumEntries; ++i)
	{
	  msg (msglev, "%s", format_route_entry (&rt->table[i], &gc));
	}
    }
  gc_free (&gc);
}

#elif defined(TARGET_LINUX)

bool
get_default_gateway (in_addr_t *gateway, in_addr_t *netmask)
{
  struct gc_arena gc = gc_new ();
  bool ret = false;
  FILE *fp = fopen ("/proc/net/route", "r");
  if (fp)
    {
      char line[256];
      int count = 0;
      int best_count = 0;
      unsigned int lowest_metric = ~0;
      in_addr_t best_gw = 0;
      while (fgets (line, sizeof (line), fp) != NULL)
	{
	  if (count)
	    {
	      unsigned int net_x = 0;
	      unsigned int mask_x = 0;
	      unsigned int gw_x = 0;
	      unsigned int metric = 0;
	      const int np = sscanf (line, "%*s\t%x\t%x\t%*s\t%*s\t%*s\t%d\t%x",
				     &net_x,
				     &gw_x,
				     &metric,
				     &mask_x);
	      if (np == 4)
		{
		  const in_addr_t net = ntohl (net_x);
		  const in_addr_t mask = ntohl (mask_x);
		  const in_addr_t gw = ntohl (gw_x);

		  dmsg (D_ROUTE_DEBUG, "GDG: route[%d] %s/%s/%s m=%u",
			count,
			print_in_addr_t ((in_addr_t) net, 0, &gc),
			print_in_addr_t ((in_addr_t) mask, 0, &gc),
			print_in_addr_t ((in_addr_t) gw, 0, &gc),
			metric);

		  if (!net && !mask && metric < lowest_metric)
		    {
		      best_gw = gw;
		      lowest_metric = metric;
		      best_count = count;
		    }
		}
	    }
	  ++count;
	}
      fclose (fp);

      if (best_gw)
	{
	  *gateway = best_gw;
	  if (netmask)
	    {
	      *netmask = 0xFFFFFF00; /* FIXME -- get the real netmask of the adapter containing the default gateway */
	    }
	  ret = true;
	}

      dmsg (D_ROUTE_DEBUG, "GDG: best=%s[%d] lm=%u",
	    print_in_addr_t ((in_addr_t) best_gw, 0, &gc),
	    best_count,
	    (unsigned int)lowest_metric);
    }

  gc_free (&gc);
  return ret;
}

#elif defined(TARGET_FREEBSD)||defined(TARGET_DRAGONFLY)

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

/* all of this is taken from <net/route.h> in FreeBSD */
#define RTA_DST     0x1
#define RTA_GATEWAY 0x2
#define RTA_NETMASK 0x4

#define RTM_GET     0x4
#define RTM_VERSION 5

#define RTF_UP      0x1
#define RTF_GATEWAY 0x2

/*
 * These numbers are used by reliable protocols for determining
 * retransmission behavior and are included in the routing structure.
 */
struct rt_metrics {
        u_long  rmx_locks;      /* Kernel must leave these values alone */
        u_long  rmx_mtu;        /* MTU for this path */
        u_long  rmx_hopcount;   /* max hops expected */
        u_long  rmx_expire;     /* lifetime for route, e.g. redirect */
        u_long  rmx_recvpipe;   /* inbound delay-bandwidth product */
        u_long  rmx_sendpipe;   /* outbound delay-bandwidth product */
        u_long  rmx_ssthresh;   /* outbound gateway buffer limit */
        u_long  rmx_rtt;        /* estimated round trip time */
        u_long  rmx_rttvar;     /* estimated rtt variance */
        u_long  rmx_pksent;     /* packets sent using this route */
        u_long  rmx_filler[4];  /* will be used for T/TCP later */
};

/*
 * Structures for routing messages.
 */
struct rt_msghdr {
        u_short rtm_msglen;     /* to skip over non-understood messages */
        u_char  rtm_version;    /* future binary compatibility */
        u_char  rtm_type;       /* message type */
        u_short rtm_index;      /* index for associated ifp */
        int     rtm_flags;      /* flags, incl. kern & message, e.g. DONE */
        int     rtm_addrs;      /* bitmask identifying sockaddrs in msg */
        pid_t   rtm_pid;        /* identify sender */
        int     rtm_seq;        /* for sender to identify action */
        int     rtm_errno;      /* why failed */
        int     rtm_use;        /* from rtentry */
        u_long  rtm_inits;      /* which metrics we are initializing */
        struct  rt_metrics rtm_rmx; /* metrics themselves */
};

struct {
  struct rt_msghdr m_rtm;
  char       m_space[512];
} m_rtmsg;

#define ROUNDUP(a) \
        ((a) > 0 ? (1 + (((a) - 1) | (sizeof(long) - 1))) : sizeof(long))

bool
get_default_gateway (in_addr_t *ret, in_addr_t *netmask)
{
  struct gc_arena gc = gc_new ();
  int s, seq, l, pid, rtm_addrs, i;
  struct sockaddr so_dst, so_mask;
  char *cp = m_rtmsg.m_space; 
  struct sockaddr *gate = NULL, *sa;
  struct  rt_msghdr *rtm_aux;

#define NEXTADDR(w, u) \
        if (rtm_addrs & (w)) {\
            l = ROUNDUP(u.sa_len); memmove(cp, &(u), l); cp += l;\
        }

#define ADVANCE(x, n) (x += ROUNDUP((n)->sa_len))

#define rtm m_rtmsg.m_rtm

  pid = getpid();
  seq = 0;
  rtm_addrs = RTA_DST | RTA_NETMASK;

  bzero(&so_dst, sizeof(so_dst));
  bzero(&so_mask, sizeof(so_mask));
  bzero(&rtm, sizeof(struct rt_msghdr));

  rtm.rtm_type = RTM_GET;
  rtm.rtm_flags = RTF_UP | RTF_GATEWAY;
  rtm.rtm_version = RTM_VERSION;
  rtm.rtm_seq = ++seq;
  rtm.rtm_addrs = rtm_addrs; 

  so_dst.sa_family = AF_INET;
  so_dst.sa_len = sizeof(struct sockaddr_in);
  so_mask.sa_family = AF_INET;
  so_mask.sa_len = sizeof(struct sockaddr_in);

  NEXTADDR(RTA_DST, so_dst);
  NEXTADDR(RTA_NETMASK, so_mask);

  rtm.rtm_msglen = l = cp - (char *)&m_rtmsg;

  s = socket(PF_ROUTE, SOCK_RAW, 0);

  if (write(s, (char *)&m_rtmsg, l) < 0)
    {
      warn("writing to routing socket");
      gc_free (&gc);
      close(s);
      return false;
    }

  do {
    l = read(s, (char *)&m_rtmsg, sizeof(m_rtmsg));
  } while (l > 0 && (rtm.rtm_seq != seq || rtm.rtm_pid != pid));
                        
  close(s);

  rtm_aux = &rtm;

  cp = ((char *)(rtm_aux + 1));
  if (rtm_aux->rtm_addrs) {
    for (i = 1; i; i <<= 1)
      if (i & rtm_aux->rtm_addrs) {
	sa = (struct sockaddr *)cp;
	if (i == RTA_GATEWAY )
	  gate = sa;
	ADVANCE(cp, sa);
      }
  }
  else
    {
      gc_free (&gc);
      return false;
    }


  if (gate != NULL )
    {
      *ret = ntohl(((struct sockaddr_in *)gate)->sin_addr.s_addr);
#if 0
      msg (M_INFO, "gw %s",
	   print_in_addr_t ((in_addr_t) *ret, 0, &gc));
#endif

      if (netmask)
	{
	  *netmask = 0xFFFFFF00; // FIXME -- get the real netmask of the adapter containing the default gateway
	}

      gc_free (&gc);
      return true;
    }
  else
    {
      gc_free (&gc);
      return false;
    }
}

#elif defined(TARGET_DARWIN)

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <net/route.h>
#include <net/if_dl.h>

struct rtmsg {
  struct rt_msghdr m_rtm;
  char       m_space[512];
};

#define ROUNDUP(a) \
        ((a) > 0 ? (1 + (((a) - 1) | (sizeof(uint32_t) - 1))) : sizeof(uint32_t))

static bool
get_default_gateway_ex (in_addr_t *ret, in_addr_t *netmask, char **ifname)
{
  struct gc_arena gc = gc_new ();
  struct rtmsg m_rtmsg;
  int s, seq, l, pid, rtm_addrs, i;
  struct sockaddr so_dst, so_mask;
  char *cp = m_rtmsg.m_space; 
  struct sockaddr *gate = NULL, *ifp = NULL, *sa;
  struct  rt_msghdr *rtm_aux;

#define NEXTADDR(w, u) \
        if (rtm_addrs & (w)) {\
            l = ROUNDUP(u.sa_len); memmove(cp, &(u), l); cp += l;\
        }

#define ADVANCE(x, n) (x += ROUNDUP((n)->sa_len))

#define rtm m_rtmsg.m_rtm

  pid = getpid();
  seq = 0;
  rtm_addrs = RTA_DST | RTA_NETMASK | RTA_IFP;

  bzero(&m_rtmsg, sizeof(m_rtmsg));
  bzero(&so_dst, sizeof(so_dst));
  bzero(&so_mask, sizeof(so_mask));
  bzero(&rtm, sizeof(struct rt_msghdr));

  rtm.rtm_type = RTM_GET;
  rtm.rtm_flags = RTF_UP | RTF_GATEWAY;
  rtm.rtm_version = RTM_VERSION;
  rtm.rtm_seq = ++seq;
  rtm.rtm_addrs = rtm_addrs; 

  so_dst.sa_family = AF_INET;
  so_dst.sa_len = sizeof(struct sockaddr_in);
  so_mask.sa_family = AF_INET;
  so_mask.sa_len = sizeof(struct sockaddr_in);

  NEXTADDR(RTA_DST, so_dst);
  NEXTADDR(RTA_NETMASK, so_mask);

  rtm.rtm_msglen = l = cp - (char *)&m_rtmsg;

  s = socket(PF_ROUTE, SOCK_RAW, 0);

  if (write(s, (char *)&m_rtmsg, l) < 0)
    {
      msg (M_WARN, "ROUTE: problem writing to routing socket");
      gc_free (&gc);
      close(s);
      return false;
    }

  do {
    l = read(s, (char *)&m_rtmsg, sizeof(m_rtmsg));
  } while (l > 0 && (rtm.rtm_seq != seq || rtm.rtm_pid != pid));
                        
  close(s);

  rtm_aux = &rtm;

  cp = ((char *)(rtm_aux + 1));
  if (rtm_aux->rtm_addrs) {
    for (i = 1; i; i <<= 1)
      {
	if (i & rtm_aux->rtm_addrs)
	  {
	    sa = (struct sockaddr *)cp;
	    if (i == RTA_GATEWAY )
	      gate = sa;
	    else if (i == RTA_IFP)
	      ifp = sa;
	    ADVANCE(cp, sa);
	  }
      }
  }
  else
    {
      gc_free (&gc);
      return false;
    }


  if (gate != NULL )
    {
      *ret = ntohl(((struct sockaddr_in *)gate)->sin_addr.s_addr);
#if 0
      msg (M_INFO, "gw %s",
	   print_in_addr_t ((in_addr_t) *ret, 0, &gc));
#endif

      if (netmask)
	{
	  *netmask = 0xFFFFFF00; // FIXME -- get the real netmask of the adapter containing the default gateway
	}

      if (ifp && ifname)
	{
	  struct sockaddr_dl *adl = (struct sockaddr_dl *) ifp;
	  char *name = malloc(adl->sdl_nlen+1);
	  check_malloc_return(name);
	  memcpy(name, adl->sdl_data, adl->sdl_nlen);
	  name[adl->sdl_nlen] = '\0';
	  *ifname = name;
	}

      gc_free (&gc);
      return true;
    }
  else
    {
      gc_free (&gc);
      return false;
    }
}

bool
get_default_gateway (in_addr_t *ret, in_addr_t *netmask)
{
  return get_default_gateway_ex(ret, netmask, NULL);
}

#elif defined(TARGET_OPENBSD) || defined(TARGET_NETBSD)

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

/* all of this is taken from <net/route.h> in OpenBSD 3.6 */
#define RTA_DST		0x1	/* destination sockaddr present */
#define RTA_GATEWAY	0x2	/* gateway sockaddr present */
#define RTA_NETMASK	0x4	/* netmask sockaddr present */

#define RTM_GET		0x4	/* Report Metrics */

#define RTM_VERSION	3	/* Up the ante and ignore older versions */

#define	RTF_UP		0x1		/* route usable */
#define	RTF_GATEWAY	0x2		/* destination is a gateway */

/*
 * Huge version for userland compatibility.
 */
struct rt_metrics {
	u_long	rmx_locks;	/* Kernel must leave these values alone */
	u_long	rmx_mtu;	/* MTU for this path */
	u_long	rmx_hopcount;	/* max hops expected */
	u_long	rmx_expire;	/* lifetime for route, e.g. redirect */
	u_long	rmx_recvpipe;	/* inbound delay-bandwidth product */
	u_long	rmx_sendpipe;	/* outbound delay-bandwidth product */
	u_long	rmx_ssthresh;	/* outbound gateway buffer limit */
	u_long	rmx_rtt;	/* estimated round trip time */
	u_long	rmx_rttvar;	/* estimated rtt variance */
	u_long	rmx_pksent;	/* packets sent using this route */
};

/*
 * Structures for routing messages.
 */
struct rt_msghdr {
	u_short	rtm_msglen;	/* to skip over non-understood messages */
	u_char	rtm_version;	/* future binary compatibility */
	u_char	rtm_type;	/* message type */
	u_short	rtm_index;	/* index for associated ifp */
	int	rtm_flags;	/* flags, incl. kern & message, e.g. DONE */
	int	rtm_addrs;	/* bitmask identifying sockaddrs in msg */
	pid_t	rtm_pid;	/* identify sender */
	int	rtm_seq;	/* for sender to identify action */
	int	rtm_errno;	/* why failed */
	int	rtm_use;	/* from rtentry */
	u_long	rtm_inits;	/* which metrics we are initializing */
	struct	rt_metrics rtm_rmx; /* metrics themselves */
};

struct {
  struct rt_msghdr m_rtm;
  char       m_space[512];
} m_rtmsg;

#define ROUNDUP(a) \
        ((a) > 0 ? (1 + (((a) - 1) | (sizeof(long) - 1))) : sizeof(long))

bool
get_default_gateway (in_addr_t *ret, in_addr_t *netmask)
{
  struct gc_arena gc = gc_new ();
  int s, seq, l, rtm_addrs, i;
  pid_t pid;
  struct sockaddr so_dst, so_mask;
  char *cp = m_rtmsg.m_space; 
  struct sockaddr *gate = NULL, *sa;
  struct  rt_msghdr *rtm_aux;

#define NEXTADDR(w, u) \
        if (rtm_addrs & (w)) {\
            l = ROUNDUP(u.sa_len); memmove(cp, &(u), l); cp += l;\
        }

#define ADVANCE(x, n) (x += ROUNDUP((n)->sa_len))

#define rtm m_rtmsg.m_rtm

  pid = getpid();
  seq = 0;
  rtm_addrs = RTA_DST | RTA_NETMASK;

  bzero(&so_dst, sizeof(so_dst));
  bzero(&so_mask, sizeof(so_mask));
  bzero(&rtm, sizeof(struct rt_msghdr));

  rtm.rtm_type = RTM_GET;
  rtm.rtm_flags = RTF_UP | RTF_GATEWAY;
  rtm.rtm_version = RTM_VERSION;
  rtm.rtm_seq = ++seq;
  rtm.rtm_addrs = rtm_addrs; 

  so_dst.sa_family = AF_INET;
  so_dst.sa_len = sizeof(struct sockaddr_in);
  so_mask.sa_family = AF_INET;
  so_mask.sa_len = sizeof(struct sockaddr_in);

  NEXTADDR(RTA_DST, so_dst);
  NEXTADDR(RTA_NETMASK, so_mask);

  rtm.rtm_msglen = l = cp - (char *)&m_rtmsg;

  s = socket(PF_ROUTE, SOCK_RAW, 0);

  if (write(s, (char *)&m_rtmsg, l) < 0)
    {
      warn("writing to routing socket");
      gc_free (&gc);
      close(s);
      return false;
    }

  do {
    l = read(s, (char *)&m_rtmsg, sizeof(m_rtmsg));
  } while (l > 0 && (rtm.rtm_seq != seq || rtm.rtm_pid != pid));
                        
  close(s);

  rtm_aux = &rtm;

  cp = ((char *)(rtm_aux + 1));
  if (rtm_aux->rtm_addrs) {
    for (i = 1; i; i <<= 1)
      if (i & rtm_aux->rtm_addrs) {
	sa = (struct sockaddr *)cp;
	if (i == RTA_GATEWAY )
	  gate = sa;
	ADVANCE(cp, sa);
      }
  }
  else
    {
      gc_free (&gc);
      return false;
    }


  if (gate != NULL )
    {
      *ret = ntohl(((struct sockaddr_in *)gate)->sin_addr.s_addr);
#if 0
      msg (M_INFO, "gw %s",
	   print_in_addr_t ((in_addr_t) *ret, 0, &gc));
#endif

      if (netmask)
	{
	  *netmask = 0xFFFFFF00; // FIXME -- get the real netmask of the adapter containing the default gateway
	}

      gc_free (&gc);
      return true;
    }
  else
    {
      gc_free (&gc);
      return false;
    }
}

#else

bool
get_default_gateway (in_addr_t *ret, in_addr_t *netmask)  /* PLATFORM-SPECIFIC */
{
  return false;
}

#endif

bool
netmask_to_netbits (const in_addr_t network, const in_addr_t netmask, int *netbits)
{
  int i;
  const int addrlen = sizeof (in_addr_t) * 8;

  if ((network & netmask) == network)
    {
      for (i = 0; i <= addrlen; ++i)
	{
	  in_addr_t mask = netbits_to_netmask (i);
	  if (mask == netmask)
	    {
	      if (i == addrlen)
		*netbits = -1;
	      else
		*netbits = i;
	      return true;
	    }
	}
    }
  return false;
}

/*
 * get_bypass_addresses() is used by the redirect-gateway bypass-x
 * functions to build a route bypass to selected DHCP/DNS servers,
 * so that outgoing packets to these servers don't end up in the tunnel.
 */

#if defined(WIN32)

static void
add_host_route_if_nonlocal (struct route_bypass *rb, const in_addr_t addr)
{
  if (test_local_addr(addr) == TLA_NONLOCAL && addr != 0 && addr != ~0) {
    int i;
    for (i = 0; i < rb->n_bypass; ++i)
      {
        if (addr == rb->bypass[i]) /* avoid duplicates */
          return;
      }
    if (rb->n_bypass < N_ROUTE_BYPASS)
      {
        rb->bypass[rb->n_bypass++] = addr;
      }
  }
}

static void
add_host_route_array (struct route_bypass *rb, const IP_ADDR_STRING *iplist)
{
  while (iplist)
    {
      bool succeed = false;
      const in_addr_t ip = getaddr (GETADDR_HOST_ORDER, iplist->IpAddress.String, 0, &succeed, NULL);
      if (succeed)
	{
	  add_host_route_if_nonlocal (rb, ip);
	}
      iplist = iplist->Next;
    }
}

static void
get_bypass_addresses (struct route_bypass *rb, const unsigned int flags)
{
  struct gc_arena gc = gc_new ();
  /*bool ret_bool = false;*/

  /* get full routing table */
  const MIB_IPFORWARDTABLE *routes = get_windows_routing_table (&gc);

  /* get the route which represents the default gateway */
  const MIB_IPFORWARDROW *row = get_default_gateway_row (routes);

  if (row)
    {
      /* get the adapter which the default gateway is associated with */
      const IP_ADAPTER_INFO *dgi = get_adapter_info (row->dwForwardIfIndex, &gc);

      /* get extra adapter info, such as DNS addresses */
      const IP_PER_ADAPTER_INFO *pai = get_per_adapter_info (row->dwForwardIfIndex, &gc);

      /* Bypass DHCP server address */
      if ((flags & RG_BYPASS_DHCP) && dgi && dgi->DhcpEnabled)
	add_host_route_array (rb, &dgi->DhcpServer);

      /* Bypass DNS server addresses */
      if ((flags & RG_BYPASS_DNS) && pai)
	add_host_route_array (rb, &pai->DnsServerList);
    }

  gc_free (&gc);
}

#else

static void
get_bypass_addresses (struct route_bypass *rb, const unsigned int flags)  /* PLATFORM-SPECIFIC */
{
}

#endif

#if AUTO_USERID || defined(ENABLE_PUSH_PEER_INFO)

#if defined(TARGET_LINUX)

bool
get_default_gateway_mac_addr (unsigned char *macaddr)
{
  struct ifreq *ifr, *ifend;
  in_addr_t ina, mask;
  struct ifreq ifreq;
  struct ifconf ifc;
  struct ifreq ifs[20]; // Maximum number of interfaces to scan
  int sd = -1;
  in_addr_t gwip = 0;
  bool ret = false;

  if (!get_default_gateway (&gwip, NULL))
    {
      msg (M_WARN, "GDGMA: get_default_gateway failed");
      goto err;
    }

  if ((sd = socket (AF_INET, SOCK_DGRAM, 0)) < 0)
    {
      msg (M_WARN, "GDGMA: socket() failed");
      goto err;
    }

  ifc.ifc_len = sizeof (ifs);
  ifc.ifc_req = ifs;
  if (ioctl (sd, SIOCGIFCONF, &ifc) < 0)
    {
      msg (M_WARN, "GDGMA: ioctl(SIOCGIFCONF) failed");
      goto err;
    }

  /* scan through interface list */
  ifend = ifs + (ifc.ifc_len / sizeof (struct ifreq));
  for (ifr = ifc.ifc_req; ifr < ifend; ifr++)
    {
      if (ifr->ifr_addr.sa_family == AF_INET)
	{
	  ina = ntohl(((struct sockaddr_in *) &ifr->ifr_addr)->sin_addr.s_addr);
	  strncpynt (ifreq.ifr_name, ifr->ifr_name, sizeof (ifreq.ifr_name));

	  dmsg (D_AUTO_USERID, "GDGMA: %s", ifreq.ifr_name);

	  /* check that the interface is up, and not point-to-point or loopback */
	  if (ioctl (sd, SIOCGIFFLAGS, &ifreq) < 0)
	    {
	      dmsg (D_AUTO_USERID, "GDGMA: SIOCGIFFLAGS(%s) failed", ifreq.ifr_name);
	      continue;
	    }

	  if ((ifreq.ifr_flags & (IFF_UP|IFF_LOOPBACK)) != IFF_UP)
	    {
	      dmsg (D_AUTO_USERID, "GDGMA: interface %s is down or loopback", ifreq.ifr_name);
	      continue;
	    }

	  /* get interface netmask and check for correct subnet */
	  if (ioctl (sd, SIOCGIFNETMASK, &ifreq) < 0)
	    {
	      dmsg (D_AUTO_USERID, "GDGMA: SIOCGIFNETMASK(%s) failed", ifreq.ifr_name);
	      continue;
	    }

	  mask = ntohl(((struct sockaddr_in *) &ifreq.ifr_addr)->sin_addr.s_addr);
	  if (((gwip ^ ina) & mask) != 0)
	    {
	      dmsg (D_AUTO_USERID, "GDGMA: gwip=0x%08x ina=0x%08x mask=0x%08x",
		    (unsigned int)gwip,
		    (unsigned int)ina,
		    (unsigned int)mask);
	      continue;
	    }
	  break;
	}
    }
  if (ifr >= ifend)
    {
      msg (M_WARN, "GDGMA: couldn't find gw interface");
      goto err;
    }

  /* now get the hardware address. */
  memset (&ifreq.ifr_hwaddr, 0, sizeof (struct sockaddr));
  if (ioctl (sd, SIOCGIFHWADDR, &ifreq) < 0)
    {
      msg (M_WARN, "GDGMA: SIOCGIFHWADDR(%s) failed", ifreq.ifr_name);
      goto err;
    }

  memcpy (macaddr, &ifreq.ifr_hwaddr.sa_data, 6);
  ret = true;

 err:
  if (sd >= 0)
    close (sd);
  return ret;
}

#elif defined(WIN32)

bool
get_default_gateway_mac_addr (unsigned char *macaddr)
{
  struct gc_arena gc = gc_new ();
  const IP_ADAPTER_INFO *adapters = get_adapter_info_list (&gc);
  in_addr_t gwip = 0;
  DWORD a_index;
  const IP_ADAPTER_INFO *ai;

  if (!get_default_gateway (&gwip, NULL))
    {
      msg (M_WARN, "GDGMA: get_default_gateway failed");
      goto err;
    }

  a_index = adapter_index_of_ip (adapters, gwip, NULL, NULL);
  ai = get_adapter (adapters, a_index);

  if (!ai)
    {
      msg (M_WARN, "GDGMA: couldn't find gw interface");
      goto err;
    }

  memcpy (macaddr, ai->Address, 6);

  gc_free (&gc);
  return true;

 err:
  gc_free (&gc);
  return false;
}

#elif defined(TARGET_DARWIN)

bool
get_default_gateway_mac_addr (unsigned char *macaddr)
{
# define max(a,b) ((a) > (b) ? (a) : (b))
  struct gc_arena gc = gc_new ();
  struct ifconf ifc;
  struct ifreq *ifr;
  char *buffer, *cp;
  bool status = false;
  in_addr_t gw = 0;
  char *ifname = NULL;
  int sockfd = -1;
  const int bufsize = 4096;

  if (!get_default_gateway_ex (&gw, NULL, &ifname)) /* get interface name of default gateway */
    {
      msg (M_WARN, "GDGMA: get_default_gateway_ex failed");
      goto done;
    }

  if (!ifname)
    {
      msg (M_WARN, "GDGMA: cannot get default gateway ifname");
      goto done;
    }

  buffer = (char *) gc_malloc (bufsize, true, &gc);

  sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd < 0)
    {
      msg (M_WARN, "GDGMA: socket failed");
      goto done;
    }

  ifc.ifc_len = bufsize;
  ifc.ifc_buf = buffer;

  if (ioctl(sockfd, SIOCGIFCONF, (char *)&ifc) < 0)
    {
      msg (M_WARN, "GDGMA: ioctl failed");
      goto done;
    }

  for (cp = buffer; cp <= buffer + bufsize - sizeof(struct ifreq); )
    {
      ifr = (struct ifreq *)cp;
      if (ifr->ifr_addr.sa_family == AF_LINK && !strncmp(ifr->ifr_name, ifname, IFNAMSIZ))
	{
	  struct sockaddr_dl *sdl = (struct sockaddr_dl *)&ifr->ifr_addr;
	  memcpy(macaddr, LLADDR(sdl), 6);
	  status = true;
	}      
      cp += sizeof(ifr->ifr_name) + max(sizeof(ifr->ifr_addr), ifr->ifr_addr.sa_len);
    }

 done:
  if (sockfd >= 0)
    close (sockfd);
  free (ifname);
  gc_free (&gc);
  return status;
# undef max
}

#else

bool
get_default_gateway_mac_addr (unsigned char *macaddr) /* PLATFORM-SPECIFIC */
{
  return false;
}

#endif
#endif /* AUTO_USERID */

/*
 * Test if addr is reachable via a local interface (return ILA_LOCAL),
 * or if it needs to be routed via the default gateway (return
 * ILA_NONLOCAL).  If the target platform doesn't implement this
 * function, return ILA_NOT_IMPLEMENTED.
 *
 * Used by redirect-gateway autolocal feature
 */

#if defined(WIN32)

int
test_local_addr (const in_addr_t addr)
{
  struct gc_arena gc = gc_new ();
  const in_addr_t nonlocal_netmask = 0x80000000L; /* routes with netmask <= to this are considered non-local */
  bool ret = TLA_NONLOCAL;

  /* get full routing table */
  const MIB_IPFORWARDTABLE *rt = get_windows_routing_table (&gc);
  if (rt)
    {
      int i;
      for (i = 0; i < rt->dwNumEntries; ++i)
	{
	  const MIB_IPFORWARDROW *row = &rt->table[i];
	  const in_addr_t net = ntohl (row->dwForwardDest);
	  const in_addr_t mask = ntohl (row->dwForwardMask);
	  if (mask > nonlocal_netmask && (addr & mask) == net)
	    {
	      ret = TLA_LOCAL;
	      break;
	    }
	}
    }

  gc_free (&gc);
  return ret;
}

#else


int
test_local_addr (const in_addr_t addr) /* PLATFORM-SPECIFIC */
{
  return TLA_NOT_IMPLEMENTED;
}

#endif
