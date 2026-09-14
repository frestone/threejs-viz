export class ConnectionAttempt {
  private nextId = 0;
  private currentId = 0;

  begin(): number {
    const id = ++this.nextId;
    this.currentId = id;
    return id;
  }

  cancel(): void {
    this.currentId = 0;
  }

  isCurrent(id: number): boolean {
    return id !== 0 && id === this.currentId;
  }
}