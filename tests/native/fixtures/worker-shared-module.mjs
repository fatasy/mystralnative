import { SharedTable } from 'mystral/shared';

const Rows = SharedTable.define('test.rows/v1', { energy: 'i32' });

self.onmessage = ({ data }) => {
    if (data.kind === 'wire-views') {
        if (!(data.bytes instanceof Uint8Array) || data.bytes.byteOffset !== 4 || data.bytes.length !== 4 ||
            !(data.words instanceof Uint16Array) || data.words.byteOffset !== 4 || data.words.length !== 2 ||
            !(data.window instanceof DataView) || data.window.byteOffset !== 5 || data.window.byteLength !== 2 ||
            data.bytes.buffer !== data.words.buffer || data.bytes.buffer !== data.window.buffer) {
            throw new Error('Transferred views did not preserve type, bounds, or backing aliasing');
        }
        if (!data.marker || data.marker.$mystralTransferredBuffer !== 0 || data.marker.note !== 'user-data') {
            throw new Error('User data collided with an internal wire marker');
        }
        if (!data.envelopeLookalike || data.envelopeLookalike.$mystralWire !== 1 ||
            data.envelopeLookalike.value[0] !== 'arraybuffer') {
            throw new Error('User data collided with the wire envelope');
        }
        if (!Array.isArray(data.clonedViews) || data.clonedViews.length === 0) {
            throw new Error('Cloned TypedArray family is missing');
        }
        const clonedBuffer = data.clonedViews[0].buffer;
        for (const view of data.clonedViews) {
            if (view.buffer !== clonedBuffer || view.length !== 1) {
                throw new Error('Cloned TypedArray backing aliasing was not preserved');
            }
        }
        data.bytes[0] += 1;
        new Uint8Array(clonedBuffer)[0] = 91;
        postMessage({
            kind: 'wire-views-result',
            bytes: data.bytes,
            words: data.words,
            window: data.window,
            clonedViews: data.clonedViews,
            marker: data.marker,
            envelopeLookalike: data.envelopeLookalike,
        }, [data.bytes.buffer, clonedBuffer]);
        return;
    }

    if (data.binary instanceof ArrayBuffer) {
        const bytes = new Uint8Array(data.binary);
        bytes[0] += 1;
        postMessage({ binary: data.binary }, [data.binary]);
        return;
    }

    const rows = Rows.attach(data.characters);
    rows.energy[0] += 41;
    postMessage({ value: rows.energy[0] });
};
