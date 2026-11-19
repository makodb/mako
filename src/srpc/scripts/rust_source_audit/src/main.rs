use quote::ToTokens;
use serde_json::{json, Value};
use std::collections::BTreeSet;
use std::path::{Path, PathBuf};
use syn::{spanned::Spanned, visit::Visit, visit_mut::VisitMut, Attribute, Item};

fn test_only(attrs: &[Attribute]) -> bool {
    attrs.iter().any(|attr| {
        attr.path().is_ident("cfg")
            && attr
                .parse_args::<syn::Path>()
                .is_ok_and(|path| path.is_ident("test"))
    })
}
struct StripDocs;
impl VisitMut for StripDocs {
    fn visit_attributes_mut(&mut self, attrs: &mut Vec<Attribute>) {
        attrs.retain(|attribute| !attribute.path().is_ident("doc"));
        for attribute in attrs {
            self.visit_attribute_mut(attribute);
        }
    }
}
fn tokens(item: &Item) -> String {
    let mut item = item.clone();
    StripDocs.visit_item_mut(&mut item);
    if let Item::Mod(module) = &mut item {
        module.content = None;
        module.semi = Some(Default::default());
    }
    item.into_token_stream().to_string()
}
fn mentions_native_marker(tokens: proc_macro2::TokenStream) -> bool {
    tokens.into_iter().any(|token| match token {
        proc_macro2::TokenTree::Ident(ident) => ident == "cpp_native_type",
        proc_macro2::TokenTree::Group(group) => mentions_native_marker(group.stream()),
        _ => false,
    })
}
fn attrs(item: &Item) -> &[Attribute] {
    match item {
        Item::Fn(value) => &value.attrs,
        Item::Struct(value) => &value.attrs,
        Item::Enum(value) => &value.attrs,
        Item::Trait(value) => &value.attrs,
        Item::Type(value) => &value.attrs,
        Item::Const(value) => &value.attrs,
        Item::Static(value) => &value.attrs,
        Item::Mod(value) => &value.attrs,
        Item::Impl(value) => &value.attrs,
        Item::Use(value) => &value.attrs,
        Item::Macro(value) => &value.attrs,
        Item::ForeignMod(value) => &value.attrs,
        _ => &[],
    }
}
fn identity(item: &Item) -> (&'static str, String) {
    match item {
        Item::Fn(value) => ("function", value.sig.ident.to_string()),
        Item::Struct(value) => ("struct", value.ident.to_string()),
        Item::Enum(value) => ("enum", value.ident.to_string()),
        Item::Trait(value) => ("trait", value.ident.to_string()),
        Item::Type(value) => ("alias", value.ident.to_string()),
        Item::Const(value) => ("const", value.ident.to_string()),
        Item::Static(value) => ("static", value.ident.to_string()),
        Item::Mod(value) => ("module", value.ident.to_string()),
        Item::Impl(value) => {
            let target = value.self_ty.to_token_stream().to_string();
            let owner = value
                .trait_
                .as_ref()
                .map(|(_, path, _)| path.to_token_stream().to_string())
                .unwrap_or_else(|| "inherent".to_owned());
            (
                "impl",
                format!("{target} as {owner} {}", value.generics.to_token_stream()),
            )
        }
        Item::Use(value) => ("use", value.tree.to_token_stream().to_string()),
        Item::Macro(value) => (
            "macro",
            value
                .ident
                .as_ref()
                .map(ToString::to_string)
                .unwrap_or_else(|| value.mac.path.to_token_stream().to_string()),
        ),
        Item::ForeignMod(value) => ("foreign", value.abi.to_token_stream().to_string()),
        _ => ("unsupported", item.to_token_stream().to_string()),
    }
}
fn inert(expression: &syn::Expr) -> bool {
    match expression {
        syn::Expr::Lit(_) => true,
        syn::Expr::Path(path) => path.path.is_ident("None"),
        syn::Expr::Tuple(tuple) => tuple.elems.is_empty(),
        syn::Expr::Unary(unary) => inert(&unary.expr),
        syn::Expr::Paren(paren) => inert(&paren.expr),
        syn::Expr::Return(ret) => ret.expr.as_ref().is_none_or(|expr| inert(expr)),
        syn::Expr::Block(block) => inert_block(&block.block),
        syn::Expr::Macro(mac) => matches!(
            mac.mac
                .path
                .segments
                .last()
                .unwrap()
                .ident
                .to_string()
                .as_str(),
            "panic" | "unreachable" | "todo" | "unimplemented"
        ),
        _ => false,
    }
}
fn inert_block(block: &syn::Block) -> bool {
    block.stmts.is_empty()
        || (block.stmts.len() == 1
            && match &block.stmts[0] {
                syn::Stmt::Expr(expr, _) => inert(expr),
                syn::Stmt::Macro(mac) => matches!(
                    mac.mac
                        .path
                        .segments
                        .last()
                        .unwrap()
                        .ident
                        .to_string()
                        .as_str(),
                    "panic" | "unreachable" | "todo" | "unimplemented"
                ),
                _ => false,
            })
}

// Record inert production function bodies and missing behavior. Test-only
// helpers do not form part of the production implementation.
#[derive(Default)]
struct CanonicalBodies {
    records: Vec<Value>,
    missing_macros: Vec<Value>,
}
impl CanonicalBodies {
    fn body(&mut self, signature: &syn::Signature, body: &syn::Block) {
        if inert_block(body) {
            self.records.push(json!({
                "name": signature.ident.to_string(),
                "line": signature.span().start().line,
                "tokens": format!("{} {}", signature.to_token_stream(), body.to_token_stream()),
            }));
        }
    }
}
impl<'ast> Visit<'ast> for CanonicalBodies {
    fn visit_item_mod(&mut self, _: &'ast syn::ItemMod) {}
    fn visit_item_fn(&mut self, item: &'ast syn::ItemFn) {
        if test_only(&item.attrs) { return; }
        self.body(&item.sig, &item.block);
        syn::visit::visit_item_fn(self, item);
    }
    fn visit_impl_item_fn(&mut self, item: &'ast syn::ImplItemFn) {
        if test_only(&item.attrs) { return; }
        self.body(&item.sig, &item.block);
        syn::visit::visit_impl_item_fn(self, item);
    }
    fn visit_trait_item_fn(&mut self, item: &'ast syn::TraitItemFn) {
        if test_only(&item.attrs) { return; }
        if let Some(body) = &item.default { self.body(&item.sig, body); }
        syn::visit::visit_trait_item_fn(self, item);
    }
    fn visit_macro(&mut self, mac: &'ast syn::Macro) {
        let name = mac.path.segments.last().unwrap().ident.to_string();
        if matches!(name.as_str(), "todo" | "unimplemented") {
            self.missing_macros.push(json!({"name":name,"line":mac.span().start().line}));
        }
        syn::visit::visit_macro(self, mac);
    }
}
struct Scanner {
    root: PathBuf,
    visited: BTreeSet<PathBuf>,
    records: Vec<Value>,
}
impl Scanner {
    fn file(&mut self, file: &Path, namespace: &str, child_dir: &Path) -> Result<(), String> {
        let physical = file
            .canonicalize()
            .map_err(|error| format!("{}: {error}", file.display()))?;
        if !physical.starts_with(&self.root) {
            return Err(format!(
                "Rust source module escapes its source directory: {}",
                file.display()
            ));
        }
        if !self.visited.insert(physical.clone()) {
            return Err(format!(
                "Rust source module is loaded more than once: {}",
                file.display()
            ));
        }
        let source = std::fs::read_to_string(&physical).map_err(|error| error.to_string())?;
        let parsed =
            syn::parse_file(&source).map_err(|error| format!("{}: {error}", file.display()))?;
        self.items(&parsed.items, namespace, &physical, child_dir)
    }
    fn items(
        &mut self,
        items: &[Item],
        namespace: &str,
        file: &Path,
        child_dir: &Path,
    ) -> Result<(), String> {
        for item in items {
            if test_only(attrs(item)) {
                continue;
            }
            let (kind, name) = identity(item);
            let path = if namespace.is_empty() {
                name.clone()
            } else {
                format!("{namespace}::{name}")
            };
            let mut canonical_bodies = CanonicalBodies::default();
            canonical_bodies.visit_item(item);
            self.records.push(json!({"kind":kind,"name":name,"path":path,"file":file,"line":item.span().start().line,
                "tokens":tokens(item),
                "native_binding":attrs(item).iter().any(|attr| mentions_native_marker(attr.to_token_stream())),
                "constant_functions":canonical_bodies.records,
                "missing_macros":canonical_bodies.missing_macros,
                "presence":attrs(item).iter().filter(|attr|attr.path().is_ident("cfg")).map(|attr|attr.to_token_stream().to_string()).collect::<Vec<_>>().join(" ")}));
            if let Item::Mod(module) = item {
                if let Some((_, content)) = &module.content {
                    self.items(
                        content,
                        &path,
                        file,
                        &child_dir.join(module.ident.to_string()),
                    )?;
                } else {
                    if module.attrs.iter().any(|attr| attr.path().is_ident("path")) {
                        return Err(format!(
                            "explicit module path is not permitted in canonical source: {path}"
                        ));
                    }
                    let direct = child_dir.join(format!("{}.rs", module.ident));
                    let nested = child_dir.join(module.ident.to_string()).join("mod.rs");
                    let target = if direct.exists() { direct } else { nested };
                    self.file(&target, &path, &child_dir.join(module.ident.to_string()))?;
                }
            }
        }
        Ok(())
    }
}
fn main() {
    let mut args = std::env::args().skip(1);
    let mode = args.next().expect("mode: canonical");
    if mode != "canonical" {
        eprintln!("unknown Rust source audit mode: {mode}");
        std::process::exit(2);
    }
    let entry = PathBuf::from(args.next().expect("entry file"));
    let namespace = args.next().unwrap_or_default();
    let root = entry
        .parent()
        .unwrap()
        .canonicalize()
        .expect("source directory");
    let mut scanner = Scanner {
        root: root.clone(),
        visited: BTreeSet::new(),
        records: Vec::new(),
    };
    if let Err(error) = scanner.file(&entry, &namespace, &root) {
        eprintln!("{error}");
        std::process::exit(2);
    }
    println!(
        "{}",
        json!({"declarations":scanner.records,"findings":[],"files":scanner.visited})
    );
}
