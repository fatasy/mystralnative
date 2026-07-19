/**
 * Canvas 2D Context
 *
 * Provides a browser-compatible CanvasRenderingContext2D implementation
 * backed by Skia for hardware-accelerated 2D graphics.
 *
 * This is the minimal API surface needed for Mystral's UI system:
 * - Text rendering: fillText, measureText, font
 * - Path drawing: beginPath, moveTo, lineTo, quadraticCurveTo, closePath, fill, stroke
 * - State: save, restore, fillStyle, strokeStyle, lineWidth, globalAlpha
 * - Rasterization: clearRect, getImageData
 */

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>

namespace mystral {
namespace canvas {

/**
 * TextMetrics - returned by measureText()
 */
struct TextMetrics {
    float width = 0;
    float actualBoundingBoxLeft = 0;
    float actualBoundingBoxRight = 0;
    float actualBoundingBoxAscent = 0;
    float actualBoundingBoxDescent = 0;
    float fontBoundingBoxAscent = 0;
    float fontBoundingBoxDescent = 0;
};

/**
 * ImageData - returned by getImageData()
 */
struct ImageData {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> data;  // RGBA pixels
};

struct CanvasDirtyRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

/**
 * Canvas2DContext - CanvasRenderingContext2D implementation
 *
 * Backed by Skia when MYSTRAL_HAS_SKIA is defined,
 * otherwise provides a stub implementation.
 */
class Canvas2DContext {
public:
    Canvas2DContext(int width, int height);
    ~Canvas2DContext();

    // Resize the canvas
    void resize(int width, int height);

    // ========================================================================
    // State Management
    // ========================================================================
    void save();
    void restore();

    // ========================================================================
    // Fill and Stroke Styles
    // ========================================================================
    void setFillStyle(const std::string& color);
    void setStrokeStyle(const std::string& color);
    void setLineWidth(float width);
    void setGlobalAlpha(float alpha);

    std::string getFillStyle() const;
    std::string getStrokeStyle() const;
    float getLineWidth() const;
    float getGlobalAlpha() const;

    // ========================================================================
    // Text
    // ========================================================================
    void setFont(const std::string& font);
    void setTextAlign(const std::string& align);
    void setTextBaseline(const std::string& baseline);

    std::string getFont() const;
    std::string getTextAlign() const;
    std::string getTextBaseline() const;

    void fillText(const std::string& text, float x, float y);
    void strokeText(const std::string& text, float x, float y);
    TextMetrics measureText(const std::string& text);

    // ========================================================================
    // Rectangles
    // ========================================================================
    void fillRect(float x, float y, float width, float height);
    void strokeRect(float x, float y, float width, float height);
    void clearRect(float x, float y, float width, float height);

    // ========================================================================
    // Paths
    // ========================================================================
    void beginPath();
    void closePath();
    void moveTo(float x, float y);
    void lineTo(float x, float y);
    void quadraticCurveTo(float cpx, float cpy, float x, float y);
    void bezierCurveTo(float cp1x, float cp1y, float cp2x, float cp2y, float x, float y);
    void arc(float x, float y, float radius, float startAngle, float endAngle, bool counterclockwise = false);
    void arcTo(float x1, float y1, float x2, float y2, float radius);
    void rect(float x, float y, float width, float height);

    void fill();
    void stroke();

    // ========================================================================
    // Transformations
    // ========================================================================
    void scale(float x, float y);
    void rotate(float angle);
    void translate(float x, float y);
    void setTransform(float a, float b, float c, float d, float e, float f);
    void transform(float a, float b, float c, float d, float e, float f);
    void resetTransform();

    // ========================================================================
    // Pixel Manipulation
    // ========================================================================
    ImageData getImageData(int x, int y, int width, int height);
    void putImageData(const ImageData& imageData, int x, int y);

    // ========================================================================
    // Canvas Dimensions
    // ========================================================================
    int getWidth() const { return width_; }
    int getHeight() const { return height_; }

    // Get raw pixel data pointer (for GPU upload)
    const uint8_t* getPixelData() const;
    size_t getPixelDataSize() const;

    bool isDirty() const { return dirty_; }
    CanvasDirtyRect getDirtyRect() const { return dirtyRect_; }
    void clearDirty();

private:
    void markDirty();
    void markDirty(int x, int y, int width, int height);

    int width_;
    int height_;
    bool dirty_ = false;
    CanvasDirtyRect dirtyRect_;

    // Skia implementation details (pimpl pattern)
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * Create JS bindings for CanvasRenderingContext2D
 *
 * This function creates a JS object with all the Canvas 2D methods
 * that wraps a native Canvas2DContext instance.
 *
 * @param engine The JS engine to create bindings in
 * @param ctx The native Canvas2DContext to wrap
 * @return JS object representing the CanvasRenderingContext2D
 */
void* createCanvas2DBindings(void* engine, Canvas2DContext* ctx);

}  // namespace canvas
}  // namespace mystral
