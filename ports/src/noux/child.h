/*
 * \brief  Noux child process
 * \author Norman Feske
 * \date   2011-02-17
 */

/*
 * Copyright (C) 2011-2012 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU General Public License version 2.
 */

#ifndef _NOUX__CHILD_H_
#define _NOUX__CHILD_H_

/* Genode includes */
#include <init/child_policy.h>
#include <base/signal.h>
#include <base/semaphore.h>
#include <cap_session/cap_session.h>
#include <os/attached_ram_dataspace.h>

/* Noux includes */
#include <file_descriptor_registry.h>
#include <vfs.h>
#include <signal_dispatcher.h>
#include <noux_session/capability.h>
#include <args.h>
#include <environment.h>
#include <local_rm_service.h>
#include <ram_session_component.h>
#include <cpu_session_component.h>


namespace Noux {

	using namespace Genode;


	/**
	 * Allocator for process IDs
	 */
	class Pid_allocator
	{
		private:

			Lock _lock;
			int  _num_pids;

		public:

			Pid_allocator() : _num_pids(0) { }

			int alloc()
			{
				Lock::Guard guard(_lock);
				return _num_pids++;
			}
	};


	/**
	 * Return singleton instance of PID allocator
	 */
	Pid_allocator *pid_allocator();


	class Child;

	bool is_init_process(Child *child);
	void init_process_exited();


	/**
	 * Signal context used for child exit
	 */
	class Child_exit_dispatcher : public Signal_dispatcher
	{
		private:

			Child *_child;

		public:

			Child_exit_dispatcher(Child *child) : _child(child) { }

			void dispatch()
			{
				if (is_init_process(_child)) {
					PINF("init process exited");

					/* trigger exit of main event loop */
					init_process_exited();
				}
			}
	};


	/**
	 * Signal context used for removing the child after having executed 'execve'
	 */
	class Child_execve_cleanup_dispatcher : public Signal_dispatcher
	{
		private:

			Child *_child;

		public:

			Child_execve_cleanup_dispatcher(Child *child) : _child(child) { }

			void dispatch()
			{
				PINF("execve cleanup dispatcher called");
				destroy(env()->heap(), _child);
			}
	};


	class Family_member : public List<Family_member>::Element
	{
		private:

			int             const _pid;
			Lock                  _lock;
			List<Family_member>   _list;
			Family_member * const _parent;
			bool                  _has_exited;
			int                   _exit_status;
			Semaphore             _wait4_blocker;

			void _wakeup_wait4()
			{
				_wait4_blocker.up();
			}

		public:

			Family_member(int pid, Family_member *parent)
			: _pid(pid), _parent(parent), _has_exited(false), _exit_status(0)
			{ }

			virtual ~Family_member() { }

			int pid() const { return _pid; }

			Family_member *parent() { return _parent; }

			int exit_status() const { return _exit_status; }

			/**
			 * Called by the parent at creation time of the process
			 */
			void insert(Family_member *member)
			{
				Lock::Guard guard(_lock);
				_list.insert(member);
			}

			/**
			 * Called by the parent from the return path of the wait4 syscall
			 */
			void remove(Family_member *member)
			{
				Lock::Guard guard(_lock);
				_list.remove(member);
			}

			/**
			 * Tell the parent that we exited
			 */
			void wakeup_parent(int exit_status)
			{
				_exit_status = exit_status;
				_has_exited  = true;
				if (_parent)
					_parent->_wakeup_wait4();
			}

			Family_member *poll4()
			{
				Lock::Guard guard(_lock);

				/* check if any of our children has exited */
				Family_member *curr = _list.first();
				for (; curr; curr = curr->next()) {
					if (curr->_has_exited)
						return curr;
				}
				return 0;
			}

			/**
			 * Wait for the exit of any of our children
			 */
			Family_member *wait4()
			{
				for (;;) {
					Family_member *result = poll4();
					if (result)
						return result;

					_wait4_blocker.down();
				}
			}
	};


	class Child : private Child_policy,
	              public  Rpc_object<Session>,
	              public  File_descriptor_registry,
	              public  Family_member
	{
		private:

			Signal_receiver *_sig_rec;

			/**
			 * Semaphore used for implementing blocking syscalls, i.e., select
			 */
			Semaphore _blocker;

			enum { MAX_NAME_LEN = 64 };
			char _name[MAX_NAME_LEN];

			Child_exit_dispatcher     _exit_dispatcher;
			Signal_context_capability _exit_context_cap;

			Child_execve_cleanup_dispatcher _execve_cleanup_dispatcher;
			Signal_context_capability       _execve_cleanup_context_cap;

			Cap_session * const _cap_session;

			enum { STACK_SIZE = 4*1024*sizeof(long) };
			Rpc_entrypoint _entrypoint;

			/**
			 * Resources assigned to the child
			 */
			struct Resources
			{
				/**
				 * Entrypoint used to serve the RPC interfaces of the
				 * locally-provided services
				 */
				Rpc_entrypoint       &ep;

				/**
				 * Registry of dataspaces owned by the Noux process
				 */
				Dataspace_registry    ds_registry;

				/**
				 * Locally-provided services for accessing platform resources
				 */
				Ram_session_component ram;
				Cpu_session_component cpu;
				Rm_session_component  rm;

				Resources(char const *label, Rpc_entrypoint &ep, bool forked)
				:
					ep(ep), ram(ds_registry), cpu(label, forked), rm(ds_registry)
				{
					ep.manage(&ram);
					ep.manage(&rm);
					ep.manage(&cpu);
				}

				~Resources()
				{
					ep.dissolve(&ram);
					ep.dissolve(&rm);
					ep.dissolve(&cpu);
				}

			} _resources;

			/**
			 * Command line arguments
			 */
			Args_dataspace _args;

			/**
			 * Environment variables
			 */
			Environment _env;

			Vfs * const _vfs;

			/**
			 * ELF binary
			 */
			Dataspace_capability const _binary_ds;

			Genode::Child _child;

			Service_registry * const _parent_services;

			Init::Child_policy_enforce_labeling _labeling_policy;
			Init::Child_policy_provide_rom_file _binary_policy;
			Init::Child_policy_provide_rom_file _args_policy;
			Init::Child_policy_provide_rom_file _env_policy;

			enum { PAGE_SIZE = 4096, PAGE_MASK = ~(PAGE_SIZE - 1) };
			enum { SYSIO_DS_SIZE = PAGE_MASK & (sizeof(Sysio) + PAGE_SIZE - 1) };

			Attached_ram_dataspace _sysio_ds;
			Sysio * const          _sysio;

			Session_capability const _noux_session_cap;

			struct Local_noux_service : public Service
			{
				Genode::Session_capability _cap;

				/**
				 * Constructor
				 *
				 * \param cap  capability to return on session requests
				 */
				Local_noux_service(Genode::Session_capability cap)
				: Service(service_name()), _cap(cap) { }

				Genode::Session_capability session(const char *args) { return _cap; }
				void upgrade(Genode::Session_capability, const char *args) { }
				void close(Genode::Session_capability) { }

			} _local_noux_service;

			Local_rm_service _local_rm_service;

			/**
			 * Exception type for failed file-descriptor lookup
			 */
			class Invalid_fd { };

			Shared_pointer<Io_channel> _lookup_channel(int fd) const
			{
				Shared_pointer<Io_channel> channel = io_channel_by_fd(fd);

				if (channel)
					return channel;

				throw Invalid_fd();
			}

			enum { ARGS_DS_SIZE = 4096 };

			/**
			 * Let specified child inherit our file descriptors
			 */
			void _assign_io_channels_to(Child *child)
			{
				for (int fd = 0; fd < MAX_FILE_DESCRIPTORS; fd++)
					if (fd_in_use(fd))
						child->add_io_channel(io_channel_by_fd(fd), fd);
			}

		public:

			/**
			 * Constructor
			 *
			 * \param forked  false if the child is spawned directly from
			 *                an executable binary (i.e., the init process,
			 *                or children created via execve, or
			 *                true if the child is a fork from another child
			 */
			Child(char const       *name,
			      Family_member    *parent,
			      int               pid,
			      Signal_receiver  *sig_rec,
			      Vfs              *vfs,
			      Args              const &args,
			      char const       *env,
			      Cap_session      *cap_session,
			      Service_registry *parent_services,
			      Rpc_entrypoint   &resources_ep,
			      bool              forked)
			:
				Family_member(pid, parent),
				_sig_rec(sig_rec),
				_exit_dispatcher(this),
				_exit_context_cap(sig_rec->manage(&_exit_dispatcher)),
				_execve_cleanup_dispatcher(this),
				_execve_cleanup_context_cap(sig_rec->manage(&_execve_cleanup_dispatcher)),
				_cap_session(cap_session),
				_entrypoint(cap_session, STACK_SIZE, "noux_process", false),
				_resources(name, resources_ep, false),
				_args(ARGS_DS_SIZE, args),
				_env(env),
				_vfs(vfs),
				_binary_ds(forked ? Dataspace_capability()
				                  : vfs->dataspace_from_file(name)),
				_child(_binary_ds, _resources.ram.cap(), _resources.cpu.cap(),
				       _resources.rm.cap(), &_entrypoint, this),
				_parent_services(parent_services),
				_labeling_policy(_name),
				_binary_policy("binary", _binary_ds,  &_entrypoint),
				_args_policy(  "args",   _args.cap(), &_entrypoint),
				_env_policy(   "env",    _env.cap(),  &_entrypoint),
				_sysio_ds(Genode::env()->ram_session(), SYSIO_DS_SIZE),
				_sysio(_sysio_ds.local_addr<Sysio>()),
				_noux_session_cap(Session_capability(_entrypoint.manage(this))),
				_local_noux_service(_noux_session_cap),
				_local_rm_service(_entrypoint, _resources.ds_registry)
			{
				_args.dump();
				strncpy(_name, name, sizeof(_name));
			}

			~Child()
			{
				PDBG("Destructing child %p", this);

				_sig_rec->dissolve(&_execve_cleanup_dispatcher);
				_sig_rec->dissolve(&_exit_dispatcher);

				/* XXX _binary_ds */

				_entrypoint.dissolve(this);
			}

			void start() { _entrypoint.activate(); }

			void start_forked_main_thread(addr_t ip, addr_t sp, addr_t parent_cap_addr)
			{
				/* poke parent_cap_addr into child's address space */
				Parent_capability const cap = _child.parent_cap();
				_resources.rm.poke(parent_cap_addr, &cap, sizeof(cap));

				/* start execution of new main thread at supplied trampoline */
				_resources.cpu.start_main_thread(ip, sp);
			}

			Ram_session_capability ram() const { return _resources.ram.cap(); }
			Rm_session_capability   rm() const { return _resources.rm.cap(); }
			Dataspace_registry &ds_registry()  { return _resources.ds_registry; }


			/****************************
			 ** Child_policy interface **
			 ****************************/

			const char *name() const { return _name; }

			Service *resolve_session_request(const char *service_name,
			                                 const char *args)
			{
				Service *service = 0;

				/* check for local ROM file requests */
				if ((service =   _args_policy.resolve_session_request(service_name, args))
				 || (service =    _env_policy.resolve_session_request(service_name, args))
				 || (service = _binary_policy.resolve_session_request(service_name, args)))
					return service;

				/* check for locally implemented noux service */
				if (strcmp(service_name, Session::service_name()) == 0)
					return &_local_noux_service;

				/*
				 * Check for the creation of an RM session, which is used by
				 * the dynamic linker to manually manage a part of the address
				 * space.
				 */
				if (strcmp(service_name, Rm_session::service_name()) == 0)
					return &_local_rm_service;

				return _parent_services->find(service_name);
			}

			void filter_session_args(const char *service,
			                         char *args, size_t args_len)
			{
				_labeling_policy.filter_session_args(service, args, args_len);
			}

			void exit(int exit_value)
			{
				PINF("child %s exited with exit value %d", _name, exit_value);

				wakeup_parent(exit_value);

				/* handle exit of the init process */
				if (parent() == 0)
					Signal_transmitter(_exit_context_cap).submit();
			}

			Ram_session *ref_ram_session()
			{
				return &_resources.ram;
			}


			/****************************
			 ** Noux session interface **
			 ****************************/

			Dataspace_capability sysio_dataspace()
			{
				return _sysio_ds.cap();
			}

			bool syscall(Syscall sc);
	};
};

#endif /* _NOUX__CHILD_H_ */
