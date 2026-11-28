// SPDX-License-Identifier: MIT
// One export list per public header produces the linked declarations, the function pointer types
// and the runtime table, so the three cannot disagree.

macro_rules! framelab_exports {
    (
        $(#[$table_meta:meta])*
        table $table:ident;
        $( $(#[$fn_meta:meta])* fn $name:ident($($arg:ident: $ty:ty),* $(,)?) -> $ret:ty; )+
    ) => {
        #[cfg(feature = "link")]
        extern "C" {
            $( $(#[$fn_meta])* pub fn $name($($arg: $ty),*) -> $ret; )+
        }

        /// Function pointer types matching the C declarations, for runtime loading.
        pub mod pfn {
            #[allow(unused_imports)]
            use super::*;
            $(
                #[allow(non_camel_case_types)]
                #[doc = concat!("Pointer to `", stringify!($name), "`.")]
                pub type $name = unsafe extern "C" fn($($ty),*) -> $ret;
            )+
        }

        /// Exported symbol names, in declaration order.
        pub const EXPORT_NAMES: &[&str] = &[$(stringify!($name)),+];

        #[cfg(feature = "link")]
        #[allow(dead_code)]
        const _: () = {
            // Compile-time check that each linked declaration has its `pfn` type.
            $( let _: pfn::$name = $name; )+
        };

        $(#[$table_meta])*
        #[cfg(feature = "runtime")]
        #[derive(Clone, Copy)]
        pub struct $table {
            $( #[doc = concat!("`", stringify!($name), "`")] pub $name: pfn::$name, )+
        }

        #[cfg(feature = "runtime")]
        impl $table {
            /// Resolves every export or reports the first one missing; `Ok(None)` when none exists.
            pub(crate) fn resolve(module: &crate::runtime::Module)
                -> Result<Option<Self>, &'static str>
            {
                let mut found = 0usize;
                let mut missing = None;
                $(
                    let $name = module.symbol(concat!(stringify!($name), "\0"));
                    if $name.is_null() {
                        missing = missing.or(Some(stringify!($name)));
                    } else {
                        found += 1;
                    }
                )+
                match missing {
                    None => Ok(Some(Self {
                        // SAFETY: each symbol is a non-null export with the C signature above.
                        $( $name: unsafe { core::mem::transmute::<*mut core::ffi::c_void, pfn::$name>($name) }, )+
                    })),
                    Some(_) if found == 0 => Ok(None),
                    Some(name) => Err(name),
                }
            }
        }
    };
}
