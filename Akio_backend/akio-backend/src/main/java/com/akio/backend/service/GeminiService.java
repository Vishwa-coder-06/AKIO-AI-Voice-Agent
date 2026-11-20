package com.akio.backend.service;

import org.springframework.http.MediaType;
import org.springframework.stereotype.Service;
import org.springframework.web.client.RestClient;

import java.util.Map;

@Service
public class GeminiService {

    private final RestClient restClient;

    private static final String MODEL = "gemini-3.5-flash-lite";
    
    public GeminiService(RestClient.Builder builder) {

        String apiKey = System.getenv("GEMINI_API_KEY");

        if (apiKey == null || apiKey.isBlank()) {
            throw new IllegalStateException(
                "GEMINI_API_KEY environment variable is not set"
            );
        }

        this.restClient = builder
                .baseUrl(
                    "https://generativelanguage.googleapis.com/v1beta"
                )
                .defaultHeader("x-goog-api-key", apiKey)
                .build();
    }

    public String generateResponse(String message) {

        // -----------------------------------------
        // AKIO voice assistant instructions
        // -----------------------------------------

    	String prompt = """
    		    You are AKIO, a friendly voice assistant.
    		    Answer naturally and concisely for speech playback.
    		    Use plain text only. No Markdown, lists, emojis or symbols.

    		    User: """ + message;

        Map<String, Object> requestBody = Map.of(
            "contents", new Object[] {
                Map.of(
                    "parts", new Object[] {
                        Map.of("text", prompt)
                    }
                )
            },
            "generationConfig",Map.of("temperature",0.4,
            		"maxOutputTokens",120
            	)
        );

        int maxRetries = 3;

        for (int attempt = 1; attempt <= maxRetries; attempt++) {

            try {
            	long startTime = System.currentTimeMillis();

                Map<?, ?> response = restClient.post()
                        .uri("/models/" + MODEL + ":generateContent")
                        .contentType(MediaType.APPLICATION_JSON)
                        .body(requestBody)
                        .retrieve()
                        .body(Map.class);

                String responseText = extractText(response);
                long elapsed = System.currentTimeMillis() - startTime;

                System.out.println(
                    "Gemini response time: " + elapsed + " ms"
                );

                // -----------------------------------------
                // Final cleanup before sending to ESP32
                // -----------------------------------------

                return cleanForSpeech(responseText);

            } catch (
                org.springframework.web.client
                    .HttpServerErrorException.ServiceUnavailable e
            ) {

                System.out.println(
                    "Gemini 503 - attempt "
                    + attempt
                    + "/"
                    + maxRetries
                );

                if (attempt == maxRetries) {

                    return "Sorry, AKIO is temporarily unavailable. Please try again.";
                }

                try {

                    long delay = 2000L * (1L << (attempt - 1));

                    System.out.println(
                        "Retrying Gemini in "
                        + delay
                        + " ms..."
                    );

                    Thread.sleep(delay);

                } catch (InterruptedException interruptedException) {

                    Thread.currentThread().interrupt();

                    return "AKIO request was interrupted.";
                }
            }
        }
        

        return "Unable to get a response from Gemini.";
    }


    // -----------------------------------------
    // Extract Gemini text
    // -----------------------------------------

    private String extractText(Map<?, ?> response) {

        var candidates =
                (java.util.List<?>) response.get("candidates");

        if (candidates == null || candidates.isEmpty()) {
            return "I couldn't generate a response.";
        }

        Map<?, ?> candidate =
                (Map<?, ?>) candidates.get(0);

        Map<?, ?> content =
                (Map<?, ?>) candidate.get("content");

        if (content == null) {
            return "I couldn't generate a response.";
        }

        var parts =
                (java.util.List<?>) content.get("parts");

        if (parts == null || parts.isEmpty()) {
            return "I couldn't generate a response.";
        }

        Map<?, ?> firstPart =
                (Map<?, ?>) parts.get(0);

        Object text =
                firstPart.get("text");

        if (text == null) {
            return "I couldn't generate a response.";
        }

        return String.valueOf(text);
    }


    // -----------------------------------------
    // Clean text for Piper / speech
    // -----------------------------------------

    private String cleanForSpeech(String text) {

        if (text == null) {
            return "I couldn't generate a response.";
        }

        return text
                // Remove Markdown bold / italic markers
                .replace("*", "")

                // Remove Markdown headings
                .replace("#", "")

                // Remove code formatting
                .replace("`", "")

                // Remove bullet characters
                .replace("•", "")

                // Remove common emoji-style symbols
                .replace("✓", "")
                .replace("✔", "")
                .replace("→", "")

                // Remove unnecessary whitespace
                .replaceAll("\\s+", " ")

                .trim();
    }
}