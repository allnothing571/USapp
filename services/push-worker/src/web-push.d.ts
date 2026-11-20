// The part of web-push 3.6.7 this worker uses: generateRequestDetails only. No @types dependency.
declare module 'web-push' {
  export interface RequestDetails {
    readonly method: string;
    readonly headers: Record<string, string | number>;
    readonly body: Buffer;
    readonly endpoint: string;
  }
  interface WebPush {
    generateRequestDetails(
      subscription: { readonly endpoint: string; readonly keys: { readonly p256dh: string; readonly auth: string } },
      payload: string,
      options: {
        readonly vapidDetails: { readonly subject: string; readonly publicKey: string; readonly privateKey: string };
        readonly TTL: number;
        readonly urgency: 'very-low' | 'low' | 'normal' | 'high';
        readonly contentEncoding: 'aes128gcm';
      },
    ): RequestDetails;
  }
  const webPush: WebPush;
  namespace webPush {
    export { RequestDetails };
  }
  export default webPush;
}
