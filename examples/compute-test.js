/**
 * Pure Compute Shader Test
 *
 * Tests GPU compute without any rendering.
 * This should work in --no-sdl mode without needing a window system.
 */

console.log("=== Pure Compute Shader Test ===");

async function main() {
    // Get adapter and device
    const adapter = await navigator.gpu.requestAdapter();
    if (!adapter) {
        throw new Error("No WebGPU adapter found");
    }
    console.log("Got WebGPU adapter");

    const device = await adapter.requestDevice();
    console.log("Got WebGPU device");

    // Create a simple compute shader that doubles numbers
    const shaderCode = `
        @group(0) @binding(0) var<storage, read> input: array<f32>;
        @group(0) @binding(1) var<storage, read_write> output: array<f32>;

        @compute @workgroup_size(64)
        fn main(@builtin(global_invocation_id) id: vec3u) {
            let i = id.x;
            if (i < arrayLength(&input)) {
                output[i] = input[i] * 2.0;
            }
        }
    `;

    const shaderModule = device.createShaderModule({ code: shaderCode });
    console.log("Created compute shader module");

    // Let Dawn compile/validate without blocking the runtime thread.
    const pipeline = await device.createComputePipelineAsync({
        layout: 'auto',
        compute: {
            module: shaderModule,
            entryPoint: 'main'
        }
    });
    console.log("Created compute pipeline");

    // Create input data
    const inputData = new Float32Array([1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16]);
    const bufferSize = inputData.byteLength;

    // Create input buffer
    const inputBuffer = device.createBuffer({
        size: bufferSize,
        usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST
    });
    device.queue.writeBuffer(inputBuffer, 0, inputData);
    console.log("Created input buffer with data:", Array.from(inputData));

    // Create output buffer
    const outputBuffer = device.createBuffer({
        size: bufferSize,
        usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC
    });

    // Create staging buffer for reading back results
    const stagingBuffer = device.createBuffer({
        size: bufferSize,
        usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST
    });

    // Create bind group
    const bindGroup = device.createBindGroup({
        layout: pipeline.getBindGroupLayout(0),
        entries: [
            { binding: 0, resource: { buffer: inputBuffer } },
            { binding: 1, resource: { buffer: outputBuffer } }
        ]
    });
    console.log("Created bind group");

    // Create command encoder and run compute pass
    const commandEncoder = device.createCommandEncoder();
    const computePass = commandEncoder.beginComputePass();
    computePass.setPipeline(pipeline);
    computePass.setBindGroup(0, bindGroup);
    computePass.dispatchWorkgroups(1);  // 1 workgroup of 64 threads
    computePass.end();

    // Copy output to staging buffer
    commandEncoder.copyBufferToBuffer(outputBuffer, 0, stagingBuffer, 0, bufferSize);

    // Submit commands
    device.queue.submit([commandEncoder.finish()]);
    console.log("Submitted compute commands");

    // This Promise now tracks native GPU completion rather than resolving at submit time.
    await device.queue.onSubmittedWorkDone();
    console.log("GPU queue work completed");

    // Read back results
    await stagingBuffer.mapAsync(GPUMapMode.READ);
    const resultData = new Float32Array(stagingBuffer.getMappedRange().slice(0));
    stagingBuffer.unmap();

    console.log("Compute results:", Array.from(resultData));

    // Verify results
    let passed = true;
    for (let i = 0; i < inputData.length; i++) {
        const expected = inputData[i] * 2;
        if (Math.abs(resultData[i] - expected) > 0.001) {
            console.error(`Mismatch at index ${i}: expected ${expected}, got ${resultData[i]}`);
            passed = false;
        }
    }

    if (passed) {
        console.log("COMPUTE TEST PASSED - All values doubled correctly!");
    } else {
        console.error("COMPUTE TEST FAILED");
    }

    // Clean up
    inputBuffer.destroy();
    outputBuffer.destroy();
    stagingBuffer.destroy();

    console.log("Compute test complete");
}

main().catch(err => {
    console.error("Compute test failed:", err.message);
});
