package com.betterspotlight.fixtures;

import java.util.List;
import java.util.ArrayList;
import java.util.stream.Collectors;

/**
 * Sample Java fixture for text extraction testing.
 * Demonstrates typical Java patterns: generics, streams, annotations.
 */
public class SampleDocument {
    private final String title;
    private final List<String> tags;

    public SampleDocument(String title) {
        this.title = title;
        this.tags = new ArrayList<>();
    }

    @Override
    public String toString() {
        return String.format("Document{title='%s', tags=%s}", title, tags);
    }

    public List<String> filterTags(String prefix) {
        return tags.stream()
                .filter(t -> t.startsWith(prefix))
                .collect(Collectors.toList());
    }

    public static void main(String[] args) {
        SampleDocument doc = new SampleDocument("Test Fixture");
        doc.tags.add("indexing");
        doc.tags.add("search");
        doc.tags.add("fts5");
        System.out.println(doc);
        System.out.println("Filtered: " + doc.filterTags("in"));
    }
}
