#!/usr/bin/env python3
"""
Merge all thesis documents into a single complete_thesis.md file.

This script:
1. Reads all thesis markdown files in the specified order
2. Adjusts heading levels for consistent hierarchy
3. Generates a comprehensive Table of Contents with anchor links
4. Updates cross-references to use anchor links within the single file
5. Preserves all formatting, code blocks, tables, diagrams
6. Injects HTML anchor IDs for duplicate headings so TOC links resolve correctly
"""

import os
import re
import sys

THESIS_DIR = os.path.dirname(os.path.abspath(__file__))

# Document order matching the task specification
CHAPTERS = [
    {
        "title": "Introduction and Reading Guide",
        "files": [("README.md", None)],  # (filename, subdirectory or None for root)
    },
    {
        "title": "Chapter 1: Mako System Overview",
        "files": [
            ("system_architecture.md", "01-mako-overview"),
            ("build_system.md", "01-mako-overview"),
        ],
    },
    {
        "title": "Chapter 2: Raft Protocol Implementation",
        "files": [
            ("protocol_overview.md", "02-raft-core"),
            ("server_implementation.md", "02-raft-core"),
            ("leader_election.md", "02-raft-core"),
            ("log_replication.md", "02-raft-core"),
            ("coordinator.md", "02-raft-core"),
            ("rpc_layer.md", "02-raft-core"),
        ],
    },
    {
        "title": "Chapter 3: Preferred Leader Election",
        "files": [
            ("design.md", "03-preferred-leader"),
            ("implementation.md", "03-preferred-leader"),
            ("testing.md", "03-preferred-leader"),
        ],
    },
    {
        "title": "Chapter 4: Mako Integration",
        "files": [
            ("architecture.md", "04-mako-integration"),
            ("raft_worker.md", "04-mako-integration"),
            ("raft_main_helper.md", "04-mako-integration"),
            ("mako_hooks.md", "04-mako-integration"),
            ("challenges.md", "04-mako-integration"),
        ],
    },
    {
        "title": "Chapter 5: Standalone Raft Testing",
        "files": [
            ("test_framework.md", "05-standalone-testing"),
            ("test_cases.md", "05-standalone-testing"),
            ("config_files.md", "05-standalone-testing"),
        ],
    },
    {
        "title": "Chapter 6: CI Integration Testing",
        "files": [
            ("ci_script.md", "06-ci-testing"),
            ("test_scenarios.md", "06-ci-testing"),
            ("example_scripts.md", "06-ci-testing"),
        ],
    },
    {
        "title": "Chapter 7: Performance Analysis",
        "files": [
            ("methodology.md", "07-performance"),
            ("results.md", "07-performance"),
            ("analysis.md", "07-performance"),
            ("figures.md", "07-performance"),
        ],
    },
    {
        "title": "Chapter 8: Log Persistence and Recovery",
        "files": [
            ("log_storage.md", "08-persistence"),
            ("recovery.md", "08-persistence"),
            ("snapshots.md", "08-persistence"),
        ],
    },
    {
        "title": "Chapter 9: Appendix",
        "files": [
            ("file_reference.md", "09-appendix"),
            ("configuration_reference.md", "09-appendix"),
            ("glossary.md", "09-appendix"),
            ("rustycpp_safety.md", "09-appendix"),
        ],
    },
]


def make_anchor(text):
    """Convert heading text to a GitHub-compatible anchor link."""
    # Strip any existing HTML tags (like <a id="...">) before making anchor
    clean = re.sub(r'<[^>]+>', '', text).strip()
    anchor = clean.lower()
    anchor = re.sub(r'[^\w\s-]', '', anchor)  # Remove non-word chars except spaces and hyphens
    anchor = re.sub(r'\s+', '-', anchor.strip())  # Replace spaces with hyphens
    anchor = re.sub(r'-+', '-', anchor)  # Collapse multiple hyphens
    return anchor


def read_file(filename, subdir):
    """Read a file and return its content."""
    if subdir:
        path = os.path.join(THESIS_DIR, subdir, filename)
    else:
        path = os.path.join(THESIS_DIR, filename)
    with open(path, 'r', encoding='utf-8') as f:
        return f.read()


def adjust_headings(content, level_offset):
    """
    Adjust heading levels in content by adding level_offset to each heading.
    Only adjusts lines that start with # (not inside code blocks).
    """
    lines = content.split('\n')
    result = []
    in_code_block = False

    for line in lines:
        # Track code block state
        if line.strip().startswith('```'):
            in_code_block = not in_code_block

        if not in_code_block and re.match(r'^#{1,6}\s', line):
            # Count current heading level
            match = re.match(r'^(#{1,6})\s(.*)', line)
            if match:
                current_hashes = match.group(1)
                heading_text = match.group(2)
                new_level = min(len(current_hashes) + level_offset, 6)
                result.append('#' * new_level + ' ' + heading_text)
            else:
                result.append(line)
        else:
            result.append(line)

    return '\n'.join(result)


def strip_related_documents_section(content):
    """Remove 'Related Documents' sections at the end of files since cross-refs are internal now."""
    # Remove trailing "Related Documents" section (## or ### level)
    # Match from the heading to the end of content
    content = re.sub(
        r'\n---\n\n#{2,3}\s+Related Documents\s*\n.*$',
        '',
        content,
        flags=re.DOTALL
    )
    return content.rstrip()


def update_cross_references(content):
    """
    Update markdown links that point to other thesis files to use anchor links.
    E.g., [text](../02-raft-core/server_implementation.md) -> [text](#server-implementation)
          [text](protocol_overview.md) -> [text](#protocol-overview)
    """
    def replace_link(match):
        text = match.group(1)
        path = match.group(2)

        # Only update links to thesis markdown files
        if not path.endswith('.md'):
            return match.group(0)

        # Skip external URLs
        if path.startswith('http://') or path.startswith('https://'):
            return match.group(0)

        # Skip links to non-thesis files (like ../../CLAUDE.md or ../../ci/ci.sh)
        if '../../' in path and 'thesis' not in path:
            return match.group(0)

        # Extract the filename without extension
        basename = os.path.basename(path)
        name_without_ext = os.path.splitext(basename)[0]

        # Convert filename to anchor
        anchor = name_without_ext.replace('_', '-').lower()

        # Handle section anchors within the file (e.g., file.md#section)
        if '#' in path:
            section = path.split('#')[1]
            return f'[{text}](#{section})'

        return f'[{text}](#{anchor})'

    # Match markdown links: [text](path)
    # But not image links: ![text](path)
    content = re.sub(r'(?<!!)\[([^\]]+)\]\(([^)]+)\)', replace_link, content)
    return content


def collect_headings_with_lines(content):
    """
    Collect all headings from content for TOC generation.
    Returns list of (level, text, line_number) tuples.
    """
    headings = []
    lines = content.split('\n')
    in_code_block = False

    for line_no, line in enumerate(lines):
        if line.strip().startswith('```'):
            in_code_block = not in_code_block
        if not in_code_block and re.match(r'^#{1,4}\s', line):
            match = re.match(r'^(#{1,4})\s+(.*)', line)
            if match:
                level = len(match.group(1))
                text = match.group(2)
                headings.append((level, text, line_no))

    return headings


def assign_unique_anchors(headings):
    """
    Given a list of (level, text, line_number) headings, assign unique anchors.
    Returns list of (level, text, line_number, anchor) tuples.
    The anchor for the first occurrence of a heading uses the base anchor.
    Subsequent duplicates get -1, -2, etc. appended (matching GitHub behavior).
    """
    anchor_count = {}
    result = []

    for level, text, line_no in headings:
        base_anchor = make_anchor(text)

        if base_anchor in anchor_count:
            anchor_count[base_anchor] += 1
            anchor = f"{base_anchor}-{anchor_count[base_anchor]}"
        else:
            anchor_count[base_anchor] = 0
            anchor = base_anchor

        result.append((level, text, line_no, anchor))

    return result


def inject_anchor_ids(content, headings_with_anchors):
    """
    For any heading that has a deduplicated anchor (i.e., anchor differs from
    the base anchor that GitHub would auto-generate), inject an HTML anchor ID
    so that the TOC link actually resolves.

    We add <a id="deduplicated-anchor"></a> right before the heading line.
    """
    lines = content.split('\n')

    # Build a map: line_number -> anchor_id to inject (only for duplicates)
    inject_map = {}
    for level, text, line_no, anchor in headings_with_anchors:
        base_anchor = make_anchor(text)
        if anchor != base_anchor:
            # This is a duplicate that got a suffix; inject an anchor tag
            inject_map[line_no] = anchor

    # Process lines in reverse order so line numbers remain valid
    for line_no in sorted(inject_map.keys(), reverse=True):
        anchor_id = inject_map[line_no]
        anchor_tag = f'<a id="{anchor_id}"></a>\n'
        lines.insert(line_no, anchor_tag)

    return '\n'.join(lines)


def generate_toc(headings_with_anchors):
    """Generate a hierarchical Table of Contents with anchor links."""
    toc_lines = []

    for level, text, line_no, anchor in headings_with_anchors:
        indent = '  ' * (level - 1)
        toc_lines.append(f'{indent}- [{text}](#{anchor})')

    return '\n'.join(toc_lines)


def build_merged_document():
    """Build the complete merged thesis document."""
    sections = []

    # Process each chapter
    for chapter in CHAPTERS:
        chapter_title = chapter["title"]

        for filename, subdir in chapter["files"]:
            content = read_file(filename, subdir)

            # Strip "Related Documents" sections
            content = strip_related_documents_section(content)

            # For README.md (introduction), adjust headings: # -> ##, ## -> ###, etc.
            if filename == "README.md" and subdir is None:
                # The README title becomes the chapter heading
                # Remove the original title line, we'll add our own chapter heading
                content = re.sub(r'^#\s+.*\n', '', content, count=1)
                content = adjust_headings(content, 1)
                content = f"# {chapter_title}\n\n{content}"
            else:
                # For regular documents:
                # Original # -> ## (document title becomes section under chapter)
                # Original ## -> ###, etc.
                content = adjust_headings(content, 1)

            # Update cross-references
            content = update_cross_references(content)

            sections.append(content)

    # Join all sections
    full_content = '\n\n---\n\n'.join(sections)

    # Collect headings with line numbers
    headings = collect_headings_with_lines(full_content)

    # Assign unique anchors (handling duplicates)
    headings_with_anchors = assign_unique_anchors(headings)

    # Inject HTML anchor IDs for duplicate headings
    full_content = inject_anchor_ids(full_content, headings_with_anchors)

    # Re-collect headings after injection (line numbers shifted)
    # We only need the (level, text, _, anchor) for TOC, which didn't change
    toc = generate_toc(headings_with_anchors)

    # Build final document
    header = """# Integrating Raft Consensus into the Mako Distributed Transaction System

## Complete Thesis Documentation

This document consolidates all thesis documentation on integrating Raft consensus into the Mako distributed transaction system. It covers the design, implementation, testing, and performance evaluation of a Raft replication module that operates as an alternative to Mako's existing Multi-Paxos atomic broadcast layer.

**Author contribution scope**: The Raft module, its integration with Mako, standalone tests, preferred leader election, and the CI test suite were implemented by the author. Mako itself (storage engine, concurrency control, transaction coordination, sharding) is pre-existing infrastructure.

---

## Table of Contents

"""

    document = header + toc + '\n\n---\n\n' + full_content

    return document


def verify_toc_links(document):
    """Verify all TOC links resolve to actual anchors in the document."""
    # Extract all TOC links
    toc_section = re.search(r'## Table of Contents\n\n(.*?)\n\n---\n', document, re.DOTALL)
    if not toc_section:
        print("WARNING: Could not find Table of Contents section")
        return 0

    toc_text = toc_section.group(1)
    toc_links = re.findall(r'\(#([^)]+)\)', toc_text)

    # Collect all heading anchors in the document body
    body_start = document.index('---\n', document.index('## Table of Contents')) + 4
    body = document[body_start:]

    # Collect anchors from headings
    heading_anchors = set()
    lines = body.split('\n')
    in_code_block = False
    for line in lines:
        if line.strip().startswith('```'):
            in_code_block = not in_code_block
        if not in_code_block and re.match(r'^#{1,6}\s', line):
            match = re.match(r'^#{1,6}\s+(.*)', line)
            if match:
                heading_anchors.add(make_anchor(match.group(1)))

    # Collect anchors from <a id="..."> tags
    html_anchors = set(re.findall(r'<a\s+id="([^"]+)"', body))

    all_anchors = heading_anchors | html_anchors

    # Check for broken links
    broken = []
    for link in toc_links:
        if link not in all_anchors:
            broken.append(link)

    if broken:
        print(f"WARNING: {len(broken)} broken TOC link(s) found:")
        for b in broken[:20]:  # Show first 20
            print(f"  - #{b}")
        if len(broken) > 20:
            print(f"  ... and {len(broken) - 20} more")
    else:
        print(f"All {len(toc_links)} TOC links verified successfully.")

    return len(broken)


def main():
    output_path = os.path.join(THESIS_DIR, 'complete_thesis.md')
    print(f"Merging thesis documents...")
    print(f"Source directory: {THESIS_DIR}")

    document = build_merged_document()

    with open(output_path, 'w', encoding='utf-8') as f:
        f.write(document)

    line_count = document.count('\n') + 1
    print(f"Written {line_count} lines to {output_path}")

    # Verify TOC links
    broken_count = verify_toc_links(document)

    if broken_count > 0:
        print(f"\nERROR: {broken_count} broken TOC links found. Please investigate.")
        sys.exit(1)
    else:
        print("\nDone. All TOC links verified.")


if __name__ == '__main__':
    main()
