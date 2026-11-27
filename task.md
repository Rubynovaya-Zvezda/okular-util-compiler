# compiler hook
i want to modify building behaviour of clang
may be even with -fplugin
to do the following:

1. add `space(z).item` that prefixes an `item` with `z_` like so: `space(namespace).item` -> `namespace_item`
2. `space().item` - should result in `item`
2. add guards `#start z` and `#end z` items within these guards should be automatically prefixed with z
3. no guard intersections shoul be allowed = should be a compilation error: `#start x #start y #end x #endy` or `#start x #start y #end y #end x` - must give a compilation error
